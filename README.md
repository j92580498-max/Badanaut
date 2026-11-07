# Badanaut — Samsung bada HLE for Android

Badanaut runs original 32-bit ARM Samsung bada applications through high-level emulation. Its Android frontend uses the PocketHLE launcher interface; the bada engine is based on [`mariotaku/bada-hle`](https://github.com/mariotaku/bada-hle). The repository also retains PocketHLE's Windows CE / Windows Mobile emulator.

## Build the Android APK

Requirements:

- JDK 17
- Android SDK Platform 34 and Build Tools 34.0.0
- Android NDK `26.1.10909125` and CMake `3.22.1`
- Stable Rust and `cargo-ndk` (`cargo install --locked cargo-ndk`)

Set `ANDROID_SDK_ROOT` to the SDK directory. If the NDK is installed somewhere else, set `ANDROID_NDK_HOME`. Then run from the repository root:

```bash
./frontends/pocket-android/build-android.sh
```

The script builds the Unicorn-backed native library for `arm64-v8a` and `armeabi-v7a`, then creates `dist/PocketHLE-Bada-Android.apk`.

## Bada game files

The game and the Bada firmware C++ runtime are intentionally not included. To build an APK with the Sky Force shortcut enabled, place a legally obtained package and compatible Bada runtime at:

```text
frontends/pocket-android/app/src/main/assets/bada/sky-force-spaces.im.zip
frontends/pocket-android/app/src/main/assets/bada/libstdc++.so.6
```

The ZIP must contain `Bin/SkyForce.exe`, `Info/Appinfo.sys`, and `Res/data.pak` at its root. The Android app extracts it into private storage. Do not commit these game or firmware files.

## Source map

- `frontends/pocket-android/` — Android launcher, SDL2 activity, and APK build.
- `frontends/pocket-android/app/src/main/cpp/` — Android JNI/native build entry and Bada integration.
- `third_party/bada-hle/` — bada HLE engine source and upstream notice.
- `crates/` — PocketHLE emulator core and Windows CE APIs.
- `docs/AGENTS.md` — PocketHLE architecture and invariants.
- `THIRD_PARTY_NOTICES.md` — component attribution and licensing boundaries.

The Bada HLE source is published here with the upstream author's permission, as communicated to the Badanaut maintainer. See `third_party/bada-hle/UPSTREAM.md`; the root PocketHLE license does not apply to that component.