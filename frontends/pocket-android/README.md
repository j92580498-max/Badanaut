# Badanaut Android

Android uses the same offline app picker as Windows and Linux, packaged from
`../shared-ui/` by Gradle. The former native library/settings screens have been
removed. Kotlin keeps package imports, runtime installation, artwork selection,
diagnostics and a separate `:bada` SDL activity. The application ID and installed
Bada library paths are preserved.

Build with JDK 17, SDK Platform and Build Tools 34, NDK `26.1.10909125` and
CMake `3.22.1`:

```sh
./frontends/pocket-android/build-android.sh
cd frontends/pocket-android
./gradlew --no-daemon --max-workers=2 :app:testDebugUnitTest
```

The script writes `dist/Badanaut-Android.apk`. CMake builds the Bada engine and
pinned Unicorn 2.1.4 directly for ARM64 and ARMv7; Rust/cargo-ndk are unnecessary.
Run `python3 tools/verify-android-apk.py dist/Badanaut-Android.apk` to verify its
native contents. Generated JNI files from older builds are ignored.

Import a Bada ZIP and select the ARM32 firmware C++ runtime separately. The
package/runtime formats and hard/soft-float requirements are documented in the
[root README](../../README.md). Existing `filesDir/bada/games/<id>` installations
remain valid. New optional metadata fields are `icon` and `last_played`.

Resolution limits the presentation area in physical pixels without changing
native device resolution or guest screen dimensions. Larger choices fit the
available screen. Scaling and touches share the same rectangle. Canvas filtering,
vsync and fullscreen options are passed to the native engine at launch. Fullscreen
is optional; the app uses normal system bars otherwise. The launch log remains
available from the shared Settings screen.

The native probes cover GLES pixels, timers, audio, ABI helpers, acceleration,
logging, desktop scaling and Android output limits. See `tests/native/`.
