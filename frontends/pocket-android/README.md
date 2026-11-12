# Badanaut Android

Android paints the native game picker with Canvas using the same reference
layout and bundled fonts as Qt on Windows and Linux. `../shared-ui/` is packaged
by Gradle. The four-column, 16-game grid is scaled and centered within the
available screen; horizontal swipes change pages, with small dots showing the current page.
The top-right ⋮ menu opens the file picker or Settings. Tap an icon to
launch; hold it for details, custom artwork or removal. TalkBack exposes each
control, including the long-click details action.

Kotlin handles package imports, runtime installation, artwork selection,
diagnostics and a separate `:bada` SDL activity. The Android application ID and namespace are `org.badanaut.emulator`. It
installs separately from the old `com.pockethle.app` application; import games
into the new app. GNU guest runtimes are bundled automatically. The Bada library directory format is unchanged. No WebView or JavaScript is used.

Build with JDK 17, SDK Platform and Build Tools 34, NDK `26.1.10909125` and
CMake `3.22.1`. Initialize native dependency submodules first:

```sh
git submodule update --init --recursive --depth 1
./frontends/pocket-android/build-android.sh
cd frontends/pocket-android
./gradlew --no-daemon --max-workers=2 :app:testDebugUnitTest
```

On Windows, run the script from Git Bash and make `pkg-config` available.
Unicorn's configuration also calls `strings`; if it is missing, set `STRINGS`
to the NDK's `toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strings.exe`.

The script writes `dist/Badanaut-Android.apk`. CMake builds the Bada engine and
pinned Unicorn 2.1.4 submodule directly for ARM64 and ARMv7. SDL's Java host
sources come from the same SDL2 submodule as its native library.
Run `python3 tools/verify-android-apk.py dist/Badanaut-Android.apk` to verify its
native contents. Generated JNI files from older builds are ignored.

Import a Bada ZIP and launch it. The bundled GNU runtime is selected from its
ARM ELF flags and attributes. Package/runtime formats are documented in the
[root README](../../README.md). Packages use `filesDir/bada/games/<id>` inside the new application storage. Optional metadata fields include `icon`, `last_played`, `api_version` and `float_abi`.

Gradle packages `../../bada_libs/` as APK assets. Launch validates and copies
both ABI sets to `filesDir/bada/bada_libs/`, preserving games and imported
runtimes. Unchanged assets are reused, and updates are checked before replacing
the previous bundle. The bundle contains C++, unwinder and exception libraries
with their GNU license and runtime-exception notices.

Optional runtime imports retain older versions and both float ABIs under
`filesDir/bada/runtimes/<sha256>/`; existing root `libstdc++.so.6` installations
remain usable. The selector also accepts ARM `libgcc_s.so.1` and `libsupc++.so`
for C++ exceptions. Launch chooses the most recently imported compatible
library for each component, with the bundle as the default. See
[GNU runtime notices and provenance](../../bada_libs/README.md).
The API version is recorded separately
from the float convention and the manifest schema version.

Presentation fits the available screen without changing guest screen dimensions.
Scaling and touches share the same rectangle. Canvas filtering, vsync and
fullscreen options are passed to the native engine at launch. Games start
fullscreen by default; turning the setting Off restores normal system bars.
Existing saved fullscreen choices are preserved. The launch log remains
available from the shared Settings screen.

The native probes cover GLES pixels, timers, audio, ABI helpers, acceleration,
logging, desktop scaling and Android presentation. See `tests/native/`.
