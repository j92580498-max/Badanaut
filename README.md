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

The script builds the Unicorn-backed native library for `arm64-v8a` and `armeabi-v7a`, then creates `dist/Badanaut-Android.apk`.

## Download an APK from GitHub Actions

Open the repository's **Actions** tab, select **Android APK**, and open a successful run. Under **Artifacts**, download **Badanaut-Android-…**, extract the ZIP, and install `Badanaut-Android.apk` on your phone.

CI builds both native engines for ARM64 and ARMv7, runs the Android unit tests, and checks the APK's native libraries and signature before uploading it. It runs on `main`, `codex/**` branches and pull requests. Once the workflow is on the default branch, use **Run workflow** to start a new build manually. APK artifacts are retained for 30 days.

## Launch a Bada game on Android

1. Open **Bada**, the default library tab.
2. Tap **Add Bada game** and select the game's ZIP package. It must contain `Bin/*.exe` (a 32-bit ARM ELF) and `Info/Appinfo.sys` or `Info/manifest.xml`, together with its resources. A containing folder inside the ZIP is supported.
3. Tap **Select runtime** and select a compatible Bada `libstdc++.so.6` or `libstdc++.so.6.0.12` from your SDK/firmware. Match its hard-float or soft-float ABI to the game.
4. Tap the installed game's card to launch it. The card menu also provides game information, runtime setup and removal.

The **Pocket PC** tab retains CAB/ZIP/RAR/Windows EXE imports and the PocketHLE runner. Bada packages use the separate bada engine. Importing a package does not guarantee game compatibility with the current HLE implementation.

## Optional bundled game files

Games and the Bada firmware C++ runtime are intentionally not included. Users can import them on the phone without rebuilding the APK. To preinstall Sky Force in a local APK, place a legally obtained package and compatible Bada runtime at:

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
