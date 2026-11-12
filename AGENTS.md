# Badanaut — project map

Badanaut emulates Samsung bada applications with a C++17 HLE engine:

- Native picker layout/fonts: `frontends/shared-ui/`, packaged unchanged on all platforms.
- Desktop Bada host: `frontends/pocket-desktop/`; native engine builds from root CMake; packaging uses Python/PySide6.
- Android bada launcher: `frontends/pocket-android/`; build with `build-android.sh`.
- Samsung bada HLE engine: `third_party/bada-hle/`; read its `README.md` and `UPSTREAM.md` before changing it.
- Native dependency configuration: `cmake/Dependencies.cmake`; pinned upstream submodules in `third_party/`. Initialize them with `git submodule update --init --recursive`. See `third_party/README.md` for versions and ownership.
- Bundled GNU guest runtimes: `bada_libs/`; preserve both float ABIs, the GPL/runtime-exception notices and file hashes. See `bada_libs/README.md` and `SOURCE.md`.

Read `docs/AGENTS.md` for frontend, packaging and validation conventions.

`THIRD_PARTY_NOTICES.md` describes component ownership and license boundaries. The six GNU GCC guest libraries explicitly listed in `bada_libs/manifest.json` are redistributed under their own terms. Game ZIPs, proprietary Bada firmware/SDK files, generated JNI libraries, and APKs remain local build inputs/outputs; do not commit them. Keep the upstream Bada attribution intact. Do not commit unless requested, and never push directly to the target branch—use a pull request.
