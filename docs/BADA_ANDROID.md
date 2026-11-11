# Android bada frontend

The Android APK hosts the same offline library and settings interface as Windows and Linux, packaged from `frontends/shared-ui/`. The old Pocket PC tab and native launcher screens have been removed. Package artwork, rounded icons, paging, search, sorting, game details and a bottom dock are shared across all hosts.

## Launch path

1. `MainActivity` imports Bada ZIP packages and their runtime through Android's document picker. Imports, listing, preparation and removal run off the UI thread.
2. `BadaPackage` extracts into a staging directory, rejecting unsafe paths, case aliases, more than 20,000 entries and more than 512 MiB of uncompressed data. It accepts one package at the ZIP root or in a containing directory, normalizes `Bin`/`Info`/`Res` and `.exe` paths, verifies an ARM32 little-endian ELF shared object and reads the AppId from XML metadata. Windows PE executables and ambiguous packages are rejected.
3. `BadaAssets` installs packages under `filesDir/bada/games/<id>` with launcher metadata. It migrates an existing `filesDir/bada/sky-force` installation into this library, keeping its resources and saves. Launch preparation reads the stored metadata and checks the executable/runtime instead of traversing all resource files again. Runtime selection installs a validated ARM32 ELF library (up to 32 MiB) as `filesDir/bada/libstdc++.so.6`. Replacing a runtime affects all Bada games, so its hard-float/soft-float ABI must match the game being launched. Failed installs clean their staging files and leave installed games/runtime intact.
4. `BadaActivity` extends SDL2's `SDLActivity`, loads `libSDL2.so`, `libunicorn.so`, and `libmain.so`, and passes the selected package path, guest runtime path and AppId to the native entry point. It runs in the `:bada` process, which exits when the activity is destroyed so each launch starts with fresh native globals.
5. The Android CMake target compiles `third_party/bada-hle/src` and links SDL2, Unicorn, GLESv2, pugixml, cxxopts, TLSF and the PowerVR texture decoder. `cmake/Dependencies.cmake` builds pinned upstream submodules, including Unicorn 2.1.4, directly for both Android ABIs. Gradle reads SDL Java sources from the SDL2 submodule and ignores legacy `jniLibs` outputs.

Display preferences in `filesDir/bada/display.json` are passed to the native engine. Presentation fits the available screen, preserving guest dimensions and aspect ratio. Rendering and touches use the same rectangle. Canvas filtering, whole-pixel scaling, vsync and fullscreen apply at the next launch. Fullscreen defaults to On and remains user-changeable; existing saved choices are preserved. New optional game metadata fields are `icon` and `last_played`; existing installations remain valid.

## Local game assets

The public repository intentionally omits proprietary game packages and the bada firmware runtime. Users normally import these on the phone. To preinstall Sky Force in a local APK, provide:

- `frontends/pocket-android/app/src/main/assets/bada/sky-force-spaces.im.zip`;
- `frontends/pocket-android/app/src/main/assets/bada/libstdc++.so.6`.

The ZIP must contain `Bin/SkyForce.exe`, `Info/Appinfo.sys`, and `Res/data.pak`. Bundled assets are installed on first library load only when present; an APK without these assets displays the normal import screen. Do not add these files to commits.

## Frontend checks

With the Android SDK and JDK configured, run from `frontends/pocket-android`:

```bash
./gradlew :app:compileDebugKotlin :app:testDebugUnitTest
```

`BadaPackageTest` uses synthetic packages and ELF headers to check import layout, metadata, traversal/alias rejection, architecture validation and copy limits. It contains no proprietary game data. Running a real game still requires an Android device and compatible game/runtime files.

## Build environment

Use JDK 17, Android SDK Platform 34, Build Tools 34.0.0, NDK 26.1.10909125, and Android CMake 3.22.1. Set `ANDROID_SDK_ROOT`, then run from the repository root:

```bash
git submodule update --init --recursive --depth 1
./frontends/pocket-android/build-android.sh
```

The output is `dist/Badanaut-Android.apk`, containing `arm64-v8a` and `armeabi-v7a` libraries. Dependency licenses from `third_party/notices.txt` are packaged under `assets/notices/`. Game and runtime assets are excluded by `.gitignore`.

`.github/workflows/android.yml` builds this APK on an x86-64 Ubuntu runner, using the same SDK/NDK/CMake versions. It runs the unit tests and `tools/verify-android-apk.py`, verifies the APK signature, and uploads APK/test-report artifacts. The APK check requires SDL2, Unicorn, Bada and the shared C++ runtime for both ABIs, requires the shared native layout/fonts, and rejects the old PocketHLE JNI engine. Desktop artifacts are built separately by `.github/workflows/desktop.yml`.

## Attribution and permission

The bada HLE source in `third_party/bada-hle/` is based on `mariotaku/bada-hle`. The Badanaut maintainer reports that the upstream author authorized publication in this repository. The upstream repository declares no general license; this specific permission does not relicense the code or grant rights for other uses. See `third_party/bada-hle/UPSTREAM.md` and `THIRD_PARTY_NOTICES.md`.
