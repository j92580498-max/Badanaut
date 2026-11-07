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

The script builds `pocket-android-jni` for `arm64-v8a` and `armeabi-v7a`, copies the matching Unicorn libraries into `jniLibs`, then runs the Gradle wrapper. Output: `dist/Badanaut-Android.apk`.

## Share a diagnostic log

Each Bada launch records device and game details, native loading messages, missing APIs, graphics errors and recent Android SDL messages. Tap **Share Bada log** over the running game (including a blank screen), or open the library menu and select **Share Bada log** after returning. Choose where to save or send the resulting text file.

Native output is limited to two 2 MB files; each launch replaces the previous session. Export a failing session before starting another game. Reports contain diagnostics, not game packages or the firmware runtime.

## Graphics validation

The Bada engine shares its GL import handlers with the desktop build. On Android it forwards them to native OpenGL ES: ES 1.1 for `glFoo_1` imports, ES 2.0 for `glFoo_2` imports and Canvas-only applications. SDL presents frames when the game calls `sglSwapBuffers`.

The native rendering probe exercises guest texture uploads, framebuffer objects, vertex arrays, indexed draws, buffer swaps and Canvas presentation using both float ABIs. It uses generated inputs, so no game package or firmware runtime is required. On Linux, install CMake, Ninja, pkg-config, SDL2, Unicorn, GLES and EGL development packages, then run:

```bash
cmake -S frontends/pocket-android/tests/native -B .build/bada-gles -G Ninja
cmake --build .build/bada-gles --parallel 2
SDL_VIDEODRIVER=offscreen LIBGL_ALWAYS_SOFTWARE=1 ctest --test-dir .build/bada-gles --output-on-failure
```

For older SDL builds without the offscreen driver, run the tests through `xvfb-run -a env SDL_VIDEODRIVER=x11 LIBGL_ALWAYS_SOFTWARE=1 ctest --test-dir .build/bada-gles --output-on-failure`, as CI does. These checks validate rendering; individual games can still require additional Bada APIs.

## Optional Bada assets

To bundle the Sky Force shortcut, put these locally under `app/src/main/assets/bada/`:

- `sky-force-spaces.im.zip` — the package ZIP with `Bin/SkyForce.exe`, `Info/Appinfo.sys`, and `Res/data.pak` at its root;
- `libstdc++.so.6` — the compatible Bada ARM32 C++ runtime.

These are copyrighted game/firmware files and are intentionally not committed. The app stores the extracted game under its private files directory. See the root `README.md` for the full source map and licensing notes.
