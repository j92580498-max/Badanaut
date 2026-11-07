# Android bada frontend

The Android APK combines the existing PocketHLE library screen with an SDL2 activity that hosts the bada HLE engine.

## Launch path

1. `MainActivity` keeps the PocketHLE game library and adds the **Sky Force (bada)** menu entry.
2. `BadaActivity` extends SDL2's `SDLActivity`, loads `libSDL2.so`, `libunicorn.so`, and `libmain.so`, and passes the package path and guest `libstdc++.so.6` path to the native entry point.
3. `BadaAssets` copies the package from app assets into private storage. It rejects path traversal, duplicate entries, excessive entry counts, and oversized archives, then checks the expected executable, metadata, and data archive.
4. The Android CMake target compiles `third_party/bada-hle/src` and links SDL2, Unicorn, GLESv2, pugixml, and cxxopts. The Rust `pocket-android-jni` crate builds Unicorn for both Android ABIs; `build-android.sh` copies those libraries into `jniLibs` before Gradle builds the APK.

Pocket PC imports continue to use the existing PocketHLE Rust runner and are not routed through the bada engine.

## Local game assets

The public repository intentionally omits the proprietary game package and bada firmware runtime. To bundle the Sky Force launch entry in a local APK, provide:

- `frontends/pocket-android/app/src/main/assets/bada/sky-force-spaces.im.zip`;
- `frontends/pocket-android/app/src/main/assets/bada/libstdc++.so.6`.

The ZIP's root must contain `Bin/SkyForce.exe`, `Info/Appinfo.sys`, and `Res/data.pak`. These files are not needed to compile the source, but the Sky Force entry cannot launch without them. Do not add them to commits.

## Build environment

Use JDK 17, Android SDK Platform 34, Build Tools 34.0.0, NDK 26.1.10909125, and Android CMake 3.22.1. Install stable Rust and `cargo-ndk`, set `ANDROID_SDK_ROOT`, then run from the repository root:

```bash
./frontends/pocket-android/build-android.sh
```

The output is `dist/PocketHLE-Bada-Android.apk`, containing `arm64-v8a` and `armeabi-v7a` libraries. Game and runtime assets are excluded by `.gitignore`.

## Attribution and permission

The bada HLE source in `third_party/bada-hle/` is based on `mariotaku/bada-hle`. The Badanaut maintainer reports that the upstream author authorized publication in this repository. The upstream repository declares no general license; this specific permission does not relicense the code or grant rights for other uses. See `third_party/bada-hle/UPSTREAM.md` and `THIRD_PARTY_NOTICES.md`.
