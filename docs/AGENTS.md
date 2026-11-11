# Badanaut frontend and build conventions

Badanaut runs Samsung bada ARM32 applications through the C++ HLE engine in
`third_party/bada-hle/`. Windows and Linux use the Python/PySide6 host in
`frontends/pocket-desktop/`; Android uses the Kotlin/SDL host in
`frontends/pocket-android/`. All hosts paint the layout and fonts from
`frontends/shared-ui/`.

Read `third_party/bada-hle/README.md` and `UPSTREAM.md` before engine changes.
Preserve upstream attribution and the component license boundaries documented
in `THIRD_PARTY_NOTICES.md`.

## Native dependencies

Use the pinned Git submodules in `third_party/`, initialized with
`git submodule update --init --recursive`. `cmake/Dependencies.cmake` configures
the same native dependencies for desktop, Android and regression probes.
Unicorn executes ARM32 guests on every host; Android host ABIs remain ARM64 and
ARMv7. Build only the PowerVR texture decoder rather than the complete SDK.
Dependency changes belong in the parent repository's CMake configuration;
keep upstream submodules clean and update their pins deliberately.

## Packages and input

Keep the Android application ID `org.badanaut.emulator` and
`filesDir/bada/games/<id>` stable for upgrades. Installed
`.badanaut-game.json` keeps its fields, including optional `icon` and
`last_played`. Desktop uses the same package metadata. Never let metadata paths
escape a package. Imports run off the UI thread and preserve resources.
A firmware runtime remains a separate ARM32 library supplied by the user.

Display preferences are presentation settings. Do not change guest bounds or
texture state to implement them. `third_party/bada-hle/src/display.h` supplies
the aspect-preserving rectangle for rendering and touch mapping. Games start
fullscreen by default on all hosts, with saved opt-outs preserved. Desktop supports F11.

## Validation and distribution

Native regression probes are in `frontends/pocket-android/tests/native/`,
desktop library tests in `frontends/pocket-desktop/tests/`, and Android JVM tests
in the app source tree. The desktop workflow builds and smoke-tests packaged
apps on Windows and Linux. The Android workflow tests native rendering, builds
both ABIs and verifies the APK. Package all redistributed dependency licenses.
Game files, firmware, generated native libraries and APKs stay outside Git.

Do not commit unless asked and never push directly to the target branch;
changes go through a pull request on a separate branch.
