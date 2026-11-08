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

This exercises the whole stack: ARM emulation, custom ELF load/reloc, the Osp
framework HLE (App/Ui/Frame/Timer with synthesized guest vtables), GL/EGL
delegation, the soft-float `__aeabi_*` runtime, a second mapped image for the
guest's own libstdc++, a case-insensitive package filesystem, and correct
multiple-inheritance thunk handling.

## Build

C++17. Dependencies: **SDL2**, **Unicorn**, and a desktop **OpenGL** driver.

```
cmake -S . -B build && cmake --build build -j
```

- **Linux / MSYS2**: install `libsdl2-dev` + `libunicorn-dev` (found via pkg-config).
- **Windows / MSVC**: provide SDL2 + Unicorn via vcpkg
  (`-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`).

The source is portable (`std::filesystem`, no POSIX-only calls); only the Linux
build is regularly tested.

## Run

```
bada-hle <app-dir-or-exe> [AppId] [maxFrames]
```

- `<app-dir>` — a bada package directory (the one containing `Signature.xml`).
  The exe (`Bin/*.exe`) and AppId (`Info/Appinfo.sys`) are resolved automatically.
- `<exe>` — a bare `.exe` (e.g. the GlesCube sample) also works.
- `[AppId]` — override (some apps' `OspMain` hash-checks `argv[0]` against it).
- `[maxFrames]` — defaults to `0` = run until the window is closed.

### The guest libstdc++ (required)

Apps call into the ARM **libstdc++ 3.4.12** they were linked against, and they
*inline* half of it — `std::string::size()`, `c_str()`, `operator[]` and the
destructor all read the COW `_Rep` header directly. So bada-hle maps the real
library as a second guest image rather than emulating it method by method.

The bada SDK ships it at
`Tools/Toolchains/ARM/arm-bada-eabi/lib/libstdc++.so.6.0.12` (soft-float) and
`.../lib/hard/armv6/vfp/libstdc++.so.6.0.12` (hard-float — what Fruit Ninja and
PvZ need). bada-hle refuses to start without one. Point it at yours with any of:

- `--libstdcxx <path>` or `BADA_LIBSTDCXX=<path>`
- `BADA_SDK=<sdk-root>` — the right multilib is picked from the app's float ABI
- copy both next to `bada-hle` as `libstdc++-hard.so` and `libstdc++-soft.so`

There is no fallback. bada-hle used to hand-roll the parts of libstdc++ apps
call — the COW string, the red-black tree, the intrusive list — but the guest
inlines enough of the library that a stand-in has to match its layout byte for
byte anyway. Mapping the real thing does that by construction, and gets
iostreams and locales for free.

The window auto-sizes to the binary's own viewport (landscape vs portrait).
