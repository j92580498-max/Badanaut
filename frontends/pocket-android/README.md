# Badanaut Android

Android paints the native game picker with Canvas using the same reference
layout and bundled fonts as Qt on Windows and Linux. `../shared-ui/` is packaged
by Gradle. The four-column, 16-game grid is scaled and centered within the
available screen; horizontal swipes change pages, with small dots showing the current page.
The top-right ⋮ menu opens the file picker, copyright info or Settings. Tap an icon to
launch; hold it for details, custom artwork or removal. TalkBack exposes each
control, including the long-click details action.

Kotlin handles package imports, runtime installation, artwork selection,
diagnostics and a separate `:bada` SDL activity. The Android application ID and namespace are `org.badanaut.emulator`. It
installs separately from the old `com.pockethle.app` application; import games
and the runtime into the new app. The Bada library directory format is unchanged. No WebView or JavaScript is used.

Build with JDK 17, SDK Platform and Build Tools 34, NDK `26.1.10909125` and
CMake `3.22.1`. Initialize native dependency submodules first:

```sh
git submodule update --init --recursive
./frontends/pocket-android/build-android.sh
cd frontends/pocket-android
./gradlew --no-daemon --max-workers=2 :app:testDebugUnitTest
```

The script writes `dist/Badanaut-Android.apk`. CMake builds the Bada engine and
pinned Unicorn 2.1.4 submodule directly for ARM64 and ARMv7. SDL's Java host
sources come from the same SDL2 submodule as its native library.
Run `python3 tools/verify-android-apk.py dist/Badanaut-Android.apk` to verify its
native contents. Generated JNI files from older builds are ignored.

Import a Bada ZIP and select the ARM32 firmware C++ runtime separately. The
package/runtime formats and hard/soft-float requirements are documented in the
[root README](../../README.md). Packages use `filesDir/bada/games/<id>` inside the new application storage. New optional metadata fields are `icon` and `last_played`.

Resolution limits the presentation area in physical pixels without changing
native device resolution or guest screen dimensions. Larger choices fit the
available screen. Scaling and touches share the same rectangle. Canvas filtering,
vsync and fullscreen options are passed to the native engine at launch. Fullscreen
is optional; the app uses normal system bars otherwise. The launch log remains
available from the shared Settings screen.

The native probes cover GLES pixels, timers, audio, ABI helpers, acceleration,
logging, desktop scaling and Android output limits. See `tests/native/`.
