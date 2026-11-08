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

The Bada engine is optimized in the debug APK. Games use their requested timer deadlines, PCM audio plays through SDL with mono/stereo resampling, and the game surface scales to the Android display while preserving its aspect ratio. Touches follow the same scaling. The activity hides system bars during play and restores immersive mode when it regains focus.

## GPU rendering on Android

OpenGL ES draws run through the phone's native GPU driver, including Mali-G57. Fruit Ninja's model buffers remain on the GPU; its interleaved sprite arrays share one upload and reuse host staging buffers. Sky Force's Canvas uses a GPU texture and shader for channel correction and display scaling. Its supported 240-pixel RGB565 frame-conversion kernel uses a native bulk operation with reused staging storage. This avoids emulating the per-pixel conversion and 2x expansion while preserving the entire guest framebuffer, including later HUD drawing. The accelerator checks the complete routine and falls back to ARM execution for changed code, overlapping buffers or unsupported memory access. The Android presenter uploads the compact 240×320 RGB565 image plus a rectangle containing subsequent HUD changes. Its shader reconstructs the guest's exact five-bit channel values and expands the image for display. A frame with no HUD changes submits 153,600 bytes instead of 1,228,800 bytes. Larger changes and devices without high-precision fragment shaders retain the full-frame upload.

Frequent 32-bit integer division imports execute as short ARM routines inside Unicorn's JIT, avoiding a host callback for each arithmetic operation. These mostly improve Sky Force's loading work; its remaining ARM software drawing and CPU emulation can still limit frame rate.

The Bada launch log records the actual GL vendor, renderer and version. On a Mali-G57 phone, the `[gl]` lines should identify ARM and Mali-G57. The software-rendering environment variables in the validation commands below apply to the Linux probes.

Android also records `[perf]` lines with frame rate, busy milliseconds per frame, the requested timer interval and the last Canvas texture upload size. These diagnostics use the log without changing the Android activity title. Desktop comparisons can enable the same output with `BADA_TRACE_FPS=1`. Set `BADA_NO_PIXEL_ACCEL=1` to compare against the original frame-conversion execution, or `BADA_NO_NATIVE_ABI=1` to compare against host arithmetic dispatch. `BADA_TRACE` disables both accelerators to retain the original trace paths.

## Share a diagnostic log

Each Bada launch records device and game details, native loading messages, missing APIs, graphics errors and recent Android SDL messages. Tap **Share Bada log** over the running game (including a blank screen), or open the library menu and select **Share Bada log** after returning. Choose where to save or send the resulting text file.

Native output is limited to two 2 MB files; each launch replaces the previous session. Export a failing session before starting another game. Reports contain diagnostics, not game packages or the firmware runtime.

## Native runtime and graphics validation

The Bada engine shares its GL import handlers with the desktop build. On Android it forwards them to native OpenGL ES: ES 1.1 for `glFoo_1` imports, ES 2.0 for `glFoo_2` imports and Canvas-only applications. SDL presents frames when the game calls `sglSwapBuffers`.

The native rendering probe exercises guest texture uploads, framebuffer objects, vertex arrays, indexed draws, mutable interleaved sprite uploads, buffer swaps, fullscreen scaling, touch mapping and Canvas color order using both float ABIs. The ES 2 Canvas probe compares all RGB565 colors and arbitrary HUD colors against the complete guest framebuffer at native and scaled display sizes, and checks texture payload reduction and full-frame fallback. A separate desktop compatibility GL probe checks the shared ES 1 guest handlers. The runtime probe checks timer deadlines, PCM buffer copying, mono/stereo resampling, volume, multiple output streams, pause/stop and Thumb listener callbacks. The integer ABI probe checks imported helper relocation, ARM/Thumb interworking, signed and unsigned results, zero divisors, signed overflow, preserved registers and elimination of host callbacks. The pixel probe compares complete native frame expansions against generated ARM code, including channel values, row padding, guard bytes, register/flag results, Thumb returns and fallback for modified code or overlapping/protected memory. These probes use generated inputs, so no game package or firmware runtime is required. On Linux, install CMake, Ninja, pkg-config, SDL2, Unicorn, desktop GL, GLES and EGL development packages, then run:

```bash
cmake -S frontends/pocket-android/tests/native -B .build/bada-gles -G Ninja
cmake --build .build/bada-gles --parallel 2
SDL_VIDEODRIVER=offscreen LIBGL_ALWAYS_SOFTWARE=1 ctest --test-dir .build/bada-gles --output-on-failure
```

For older SDL builds without the offscreen driver, run the tests through `xvfb-run -a env SDL_VIDEODRIVER=x11 LIBGL_ALWAYS_SOFTWARE=1 ctest --test-dir .build/bada-gles --output-on-failure`, as CI does. Some Mesa software drivers advertise ES 1 support but fail to create its context with `EGL_BAD_ALLOC`; the ES 1 probes report a CTest skip for that host failure before any guest code runs. ES 2, desktop compatibility GL, runtime and logging checks remain required, and rendering failures after context creation still fail the suite. Native ES 1 rendering and Android device behavior require verification on a host/device with a working ES 1 driver.

## Optional Bada assets

To bundle the Sky Force shortcut, put these locally under `app/src/main/assets/bada/`:

- `sky-force-spaces.im.zip` — the package ZIP with `Bin/SkyForce.exe`, `Info/Appinfo.sys`, and `Res/data.pak` at its root;
- `libstdc++.so.6` — the compatible Bada ARM32 C++ runtime.

These are copyrighted game/firmware files and are intentionally not committed. The app stores the extracted game under its private files directory. See the root `README.md` for the full source map and licensing notes.
