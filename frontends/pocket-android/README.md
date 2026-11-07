# PocketHLE Android frontend

The Android app provides the PocketHLE game-library interface and a Samsung bada launch entry backed by `third_party/bada-hle`. It supports CAB/ZIP/RAR/SFX imports for Pocket PC games; the Bada Sky Force package is supplied separately.

## Requirements

- JDK 17
- Android SDK Platform 34 and Build Tools 34.0.0
- Android NDK `26.1.10909125`
- Android CMake `3.22.1`
- Stable Rust and `cargo-ndk`

## Build the APK

From the repository root, run:

```bash
./frontends/pocket-android/build-android.sh
```

The script builds `pocket-android-jni` for `arm64-v8a` and `armeabi-v7a`, copies the matching Unicorn libraries into `jniLibs`, then runs the Gradle wrapper. Output: `dist/PocketHLE-Bada-Android.apk`.

## Optional Bada assets

To bundle the Sky Force shortcut, put these locally under `app/src/main/assets/bada/`:

- `sky-force-spaces.im.zip` — the package ZIP with `Bin/SkyForce.exe`, `Info/Appinfo.sys`, and `Res/data.pak` at its root;
- `libstdc++.so.6` — the compatible Bada ARM32 C++ runtime.

These are copyrighted game/firmware files and are intentionally not committed. The app stores the extracted game under its private files directory. See the root `README.md` for the full source map and licensing notes.