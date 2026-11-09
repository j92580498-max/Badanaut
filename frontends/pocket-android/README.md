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
[root README](../../README.md). Packages use `filesDir/bada/games/<id>` inside the new application storage. New optional metadata fields are `icon` and `last_played`.

Screen resolution selects the EGL render buffer size. Hold a game, open
**Screen resolution** in Game details and choose a preset to save it for that
game. Settings supplies the default for games without their own choice.
**Game default** uses the native size from the manifest and Bada Form/Frame or
first full-surface GL viewport. Older ES1 SDK logical window dimensions also
resolve games that rotate landscape content inside a portrait framebuffer.
Landscape games swap the preset's width and
height. The activity locks to the game's orientation, and the surface fills
the available screen with uniform scaling and cropped edges. Both scaling
modes fill the screen; whole-pixel scaling rounds up. Touches are mapped through
the surface scale and the native crop, including each simultaneous pointer.
The inverse presentation rotation preserves the game's own touch conventions.
Launch logs report the selected, native, requested and actually applied sizes.
Canvas filtering,
vsync and fullscreen options are passed to the native engine at launch. Fullscreen
is optional; the app uses normal system bars otherwise. The launch log remains
available from the shared Settings screen.

Game volume buttons control Android's media stream. The recognized Megal/Bada
audio path used by N.O.V.A. compensates for its native master-volume attenuation
before the guest mixes its sounds. Its saved volume sliders and mute still work;
the PCM output for other games keeps unity gain. The applied master gain is logged.

The native probes cover GLES pixels, timers, audio, ABI helpers, acceleration,
logging, rotation, desktop scaling and Android buffer sizes. See `tests/native/`.
