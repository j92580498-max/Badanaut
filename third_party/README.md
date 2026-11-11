# Native dependencies

The Bada engine in `bada-hle/` remains part of Badanaut so local HLE changes can
be reviewed in this repository. Read its `README.md` and `UPSTREAM.md` before
changing it. The other directories are unmodified upstream Git submodules.

```sh
git clone --recurse-submodules https://github.com/j92580498-max/Badanaut.git
cd Badanaut
```

For an existing clone, run `git submodule update --init --recursive` from the
repository root. Git records exact commits rather than following upstream
branches. Recursive checkout also initializes dependencies nested inside
SDL2_image. CI uses `actions/checkout` with `submodules: recursive`.

| Submodule | Upstream version / commit | Use |
| --- | --- | --- |
| [SDL2](https://github.com/libsdl-org/SDL) | 2.32.10 — `5d249570393f7a37e037abf22cd6012a4cc56a71` | Window, input, audio and Android Java host |
| [SDL2_image](https://github.com/libsdl-org/SDL_image) | 2.8.8 — `c1bf2245b0ba63a25afe2f8574d305feca25af77` | Desktop PNG/JPEG screenshots |
| [Unicorn](https://github.com/unicorn-engine/unicorn) | 2.1.4 — `8028ec436f2d9376525352dd38ed9ed6b9f6be10` | ARM32 CPU execution on every host |
| [cxxopts](https://github.com/jarro2783/cxxopts) | 3.1.1 — `eb787304d67ec22f7c3a184ee8b4c481d04357fd` | Command-line options |
| [pugixml](https://github.com/zeux/pugixml) | 1.13 — `a0e064336317c9347a91224112af9933598714e9` | Bada package XML metadata |
| [PowerVR Native SDK](https://github.com/powervr-graphics/Native_SDK) | `2b1bf2f14d3365d0bb801e2a6a131a319d3a2e48` | Only `framework/PVRCore/texture/PVRTDecompress.cpp` and its header are used |
| [TLSF](https://github.com/mattconte/tlsf) | `deff9ab509341f264addbd3c8ada533678591905` | Shared guest heap allocator |
| [dr_libs](https://github.com/mackron/dr_libs) | MP3 0.7.5 / WAV 0.14.7 — `4ffbded724cc3e3d2838eebca70858bf5984385a` | Memory MP3/WAV decoders for Osp Media Player |
| [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) | 4.13.3 + upstream changes — `0dbc52f86404f9e1f26ce0df3015ed23ff54a726` | Desktop Bada control overlay and SDL/OpenGL 2 backend |

`cmake/Dependencies.cmake` configures these sources for desktop, Android and
native regression probes. No native dependency is downloaded by CMake.
The Windows x64 build compiles a generated copy of Unicorn's JIT allocation
source with corrected exception-callback stack alignment and bounded page
commits. `cmake/UnicornWindows.cmake` applies these fixes in the build directory;
the pinned submodule stays unchanged. The `windows-unicorn-cache` CTest probe
exercises small caches, growth beyond one commit batch and cache recycling.
The PowerVR pin precedes its switch to private SSH dependencies. Its two
decoder files match the previously vendored `63f0098` snapshot exactly.
SDL2_image uses its built-in PNG/JPEG codecs; optional external image formats
are disabled. Android obtains SDL Java sources directly from the same SDL2
submodule used by its native build. Native runtime libraries are copied beside
the desktop engine for packaging.

Only the MP3 and WAV memory decoders from dr_libs are compiled, through
`cmake/audio_decoders.c`. Encoded Player input is bounded and owned by the host;
the shared SDL mixer decodes fixed PCM blocks without accessing the guest.
The same decoder is used on Android, Windows and Linux.

When updating a pin, keep the dependency worktree clean, review upstream changes
and run the relevant builds. Keep the Bada engine's upstream attribution and
all dependency licenses. `notices.txt` lists the files copied into desktop and
Android distribution notices; `nuklear.h` retains the embedded font and stb
notices as well as Nuklear's own license.
