# bada-hle

A **high-level emulator (HLE)** for Samsung bada apps. It loads the *original*
ARM32 bada executable (an ELF shared object), emulates the ARM CPU with
[Unicorn](https://www.unicorn-engine.org/), and provides host-side
implementations of the bada framework APIs — instead of reverse-engineering and
re-porting the app's logic by hand.

This is the *opposite* approach to a source port: the goal is to run the
unmodified binary by faithfully emulating only the **OS/library boundary**.

## How it works

```
  App.exe (ARM32 ELF, DYN)
        |  custom loader: map PT_LOAD, relocate, bind imports
        v
  Unicorn ARM core  ──(calls imported symbol)──>  HLE trampoline page
        |                                              |
        | host GL context (SDL2, compat profile)       v
        v                                       host C++ dispatch
   on-screen window                          ┌── libc/libm  -> host runtime
                                             ├── glFoo_1/_2  -> host desktop GL
                                             └── Osp::*      -> HLE impls
```

Most imported symbols are bound to a 4-byte **trampoline slot** (`bx lr`). A
`UC_HOOK_CODE` over that page catches the call, runs a host C++ handler that
reads args from the guest regs (AAPCS hard-float/VFP **or** soft-float, detected
per binary), and writes the result back — then `bx lr` returns to the guest. See
`src/guest.h` for the address map.

The four 32-bit integer division helpers (`__aeabi_uidiv`, `__aeabi_idiv`,
`__aeabi_uidivmod`, `__aeabi_idivmod`) run as short ARM routines on a separate
guest page inside Unicorn's JIT. This avoids host dispatch for frequent integer
arithmetic. `BADA_NO_NATIVE_ABI=1` restores the host handlers for comparisons;
`BADA_TRACE` also uses them so import tracing includes arithmetic calls.

The guest heap uses the unmodified TLSF pool allocator in host-owned memory
shared with Unicorn. Recycled payloads retain the original cleared-allocation
behavior used by Fruit Ninja. Freed temporary buffers can be reused, and
immediate GL uploads can read the heap directly. A persistent watchdog waits until each
guest callback's deadline or cancellation; it retains the 120-second boot
guard without Unicorn's per-call thread creation and two-microsecond polling.
See [NOVA.md](NOVA.md) for the Nova-specific causes, sources and validation.

SDL multi-touch is forwarded through the Bada listener with a stable point ID
for each finger. Mouse input stays independent, and focus loss releases held
contacts. `Touch::SetMultipointEnabled` controls delivery of additional fingers.

Verified pixel-copy kernels can also use native acceleration (`src/pixel_accel.cpp`).
The supported RGB565-to-XRGB8888 2x expansion preserves the full guest framebuffer,
pointer advances, saved registers and flags. It checks every instruction in the
routine and retains ARM execution for changed code or unsupported inputs.
`BADA_NO_PIXEL_ACCEL=1` and `BADA_TRACE` retain the original pixel-copy execution.
The Android ES 2 presenter can upload the original RGB565 rows plus later Canvas
changes as a separate rectangle; its shader preserves the guest conversion
colors and scales both layers on the same pixel grid. Large changes or missing
fragment high precision use the ordinary full-frame upload.

The imports split into three buckets:

| Bucket | Strategy |
|---|---|
| `Osp::*` framework | HLE — the real work (`src/hle/osp.cpp`, `src/hle/io.cpp`) |
| GL ES 1.x/2.0 + EGL (`glFoo_1`/`_2`, `sgl*`) | forward to host desktop GL / SDL (`src/hle/gl.cpp`) |
| libc / libm / `__aeabi_*` / heap | host runtime (`src/hle/libc.cpp`, `src/hle/io.cpp`); hot integer division helpers in the ARM JIT |
| C++ runtime (`std::*`) | the guest's own libstdc++.so, mapped as a second image (required) |

## Status

Runs the **unmodified ARM binaries** end to end:

- **Fruit Ninja** (hard-float, GL ES 1.x) boots through its full lifecycle and
  draws its Halfbrick splash/loading screen in correct 800×480 landscape, then
  runs an indefinite frame loop with no crashes and no unported functions.
- **GlesCube** sample (soft-float, GL ES 2.0) draws a depth-tested, color-
  interpolated rotating 3D cube (GLSL compiled from source, client arrays
  translated guest→VBO/VAO).
- **3D Tower Bloxx Deluxe** (hard-float, GL ES 2.0) boots to its title and mode
  menus, enters Quick Game and accepts touch input to drop blocks. The
  MP3 soundtrack and WAV effects play through the shared SDL mixer. The
  [compatibility notes](TOWER_BLOXX.md) cover its startup, frame-time and music fixes.
- **3D Rollercoaster Rush Jurassic** (hard-float, GL ES 2.0) reaches its title,
  settings and Career map, enters a 3D track and accepts acceleration, braking
  and restart input. Music and effects play when enabled in Settings. The
  [compatibility notes](ROLLERCOASTER_RUSH.md) cover its event thread, timer
  dispatch and verification on a restricted software-rendering host.
- **Exploration 1.3.1** (soft-float, GL ES 1.x) boots, renders terrain and its
  menus, and reopens a saved world. The [compatibility notes](EXPLORATION.md)
  describe its framework, file, mesh and Windows JIT fixes and validation limits.
- **Angry Birds 1.6.3** (bada API 2.0, hard-float) reaches its menus and playable
  levels with touch input, sound and normal idle animations. The
  [compatibility notes](ANGRY_BIRDS.md) cover exception support, complete font
  resources, runtime selection and the bada random-number range.

This exercises the whole stack: ARM emulation, custom ELF load/reloc, the Osp
framework HLE (App/Ui/Frame/Timer with synthesized guest vtables), GL/EGL
delegation, the soft-float `__aeabi_*` runtime, a second mapped image for the
guest's own libstdc++, a case-insensitive package filesystem, and correct
multiple-inheritance thunk handling.

## Build

C++17. SDL2, SDL2_image, Unicorn and most support libraries are pinned Git
submodules in the parent Badanaut repository. The small PowerVR decoder is
vendored in `third_party/pvr/`. A desktop OpenGL driver is
required. From the Badanaut repository root:

```
git submodule update --init --recursive --depth 1
cmake -S . -B build
cmake --build build --config Release --parallel 2
```

- **Linux**: install a C/C++ compiler, CMake and OpenGL/audio/window development
  headers; see `../../frontends/pocket-desktop/README.md`.
- **Windows / MSVC**: use Visual Studio's C++ tools. Native libraries build
  from the submodules and are copied beside `build/native/bada-hle.exe`.

The source is portable (`std::filesystem`, no POSIX-only calls). Badanaut's CI
builds and packages the engine on Windows and Linux. The Android host builds
the same HLE sources with its GLES presenter; see
`../../frontends/pocket-android/README.md`.

## Run

```
bada-hle <app-dir-or-exe> [AppId] [maxFrames]
```

- `<app-dir>` — a bada package directory (the one containing `Signature.xml`).
  The exe (`Bin/*.exe`) and AppId (`Info/Appinfo.sys`) are resolved automatically.
- `<exe>` — a bare `.exe` (e.g. the GlesCube sample) also works.
- `[AppId]` — override (some apps' `OspMain` hash-checks `argv[0]` against it).
- `[maxFrames]` — defaults to `0` = run until the window is closed.

### Bundled guest C++ runtimes

Apps call into ARM **libstdc++**, and they
*inline* half of it — `std::string::size()`, `c_str()`, `operator[]` and the
destructor all read the COW `_Rep` header directly. So bada-hle maps the real
library as a second guest image rather than emulating it method by method.

Normal builds include `bada_libs/soft-float/` and `bada_libs/hard-float/` beside
the engine. Each contains GNU GCC 4.5.3 `libstdc++.so.6`, `libgcc_s.so.1` and
`libsupc++.so`. The engine finds that bundle automatically; under `.build/`
it can also find the repository's bundle without relying on the current
working directory. CMake and desktop/Android packaging retain the same
libraries and GNU license notices. See [bundle provenance](../../bada_libs/README.md).

For development or a different compatible runtime, overrides remain available:

- `--libstdcxx <path>` or `BADA_LIBSTDCXX=<path>`

If a compatible bundle is unavailable, the legacy search also checks the
package, loose `libstdc++-hard.so` / `libstdc++-soft.so` files next to the
engine, and `BADA_SDK=<sdk-root>` with the app's matching multilib.

API 1.2 and 2.0 games can use different float calling conventions. The loader
checks ARM ELF flags and attributes rather than inferring the ABI from the API version or
filename. The desktop and Android hosts retain imported runtime versions and
select the most recently imported compatible library; the older runtime is
kept alongside newer SDK libraries.

Apps that throw C++ exceptions can additionally map ARM `libgcc_s.so.1` with
`--libgcc <path>` or `BADA_LIBGCC`. The SDK 2.0 C++ library also needs exception
ABI support from `libsupc++.so`, supplied with `--libsupcxx <path>` or
`BADA_LIBSUPCXX`. Matching companions are loaded automatically from the bundle,
or searched beside an overridden C++ runtime, in the
package's `Bin`, beside the engine, and in `BADA_SDK`'s matching multilib.
Explicitly supplied libraries must match the game's float ABI.
See [Angry Birds runtime setup](ANGRY_BIRDS.md#runtime-and-package-requirements)
for package requirements and optional SDK runtime reproduction.

There is no synthetic C++ runtime fallback. bada-hle used to hand-roll the parts of libstdc++ apps
call — the COW string, the red-black tree, the intrusive list — but the guest
inlines enough of the library that a stand-in has to match its layout byte for
byte anyway. Mapping the real thing does that by construction, and gets
iostreams and locales for free.

The window auto-sizes to the binary's own viewport (landscape vs portrait).
