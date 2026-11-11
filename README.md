# Badanaut — Samsung bada emulator

Badanaut runs original 32-bit ARM Samsung bada applications through high-level
emulation with a C++17 engine.

## Download builds

In **Actions**, open a successful **Desktop builds** or **Android APK** run and
download its artifact:

| Platform | Artifact contents | Start |
| --- | --- | --- |
| Windows x86-64 | `Badanaut-Windows-x64.zip` | Extract everything; run `Badanaut/Badanaut.exe` |
| Linux x86-64 | `Badanaut-Linux-x64.tar.gz` | Extract everything; run `./Badanaut/Badanaut` |
| Android ARM64/ARMv7 | `Badanaut-Android.apk` | Install the APK |

Workflows run on `main`, `codex/**`, pull requests and manual dispatch. Desktop
builds include the native Bada engine and Qt host; Android includes its native
libraries and shared UI. Artifacts are kept for 30 days. Linux packages are built
on Ubuntu 22.04 and require a compatible glibc and OpenGL driver.

## Add and play games

1. Open the **⋮ menu → Open file manager** and select a Bada ZIP containing one `Bin/*.exe` ARM32
   ELF and `Info/Appinfo.sys` or `Info/manifest.xml`, along with its resources.
   A containing directory inside the ZIP is supported.
2. Open **Settings → Select Bada runtime** and select an original ARM32 Bada `libstdc++.so.6`
   or `libstdc++.so.6.0.12` from your SDK/firmware. Its hard/soft-float ABI must
   match the game. Host Windows, Linux and Android libraries are incompatible.
3. Click or tap the game's icon. Right-click or hold an icon to open
   details, choose artwork or remove the installed package and its saves.

Packages, runtime and saves stay in local application storage. Android uses the package name
`org.badanaut.emulator`, which installs separately from the former
`com.pockethle.app` app. Import games and the runtime into the new app. Importing a
package does not guarantee compatibility with the current HLE implementation.
Game files and firmware are not included in builds and must not be committed.

## Display settings

The desktop library opens in a resizable window. Games start **fullscreen by
default** on all platforms; the setting remains user-changeable. **F11** toggles
fullscreen on desktop. **Settings** offers:

- Fit or whole-pixel scaling, preserving aspect ratio.
- Sharp or smooth Canvas filtering and vertical sync.
- Fullscreen or windowed games.

Presentation follows each game's orientation and fits the available screen.
The guest always retains its original dimensions.
Whole-pixel scaling falls back to fitting when the screen is smaller than the
guest. Filtering applies to Canvas output; the game controls its own GL textures.
Vsync depends on driver support. Changes apply at the next game launch.

## Build

Clone the repository and its pinned native dependencies:

```sh
git clone --recurse-submodules https://github.com/j92580498-max/Badanaut.git
cd Badanaut
```

For an existing clone, run `git submodule update --init --recursive`.

**Desktop:** Python 3.12, CMake 3.22+, OpenGL and a C/C++17 compiler.
Windows uses Visual Studio; SDL2, SDL2_image and the CPU engine build from
the submodules. See
[desktop build instructions](frontends/pocket-desktop/README.md).

**Android:** JDK 17, Android SDK Platform/Build Tools 34, NDK `26.1.10909125`
and CMake `3.22.1`. Set `ANDROID_SDK_ROOT`, then run:

```sh
./frontends/pocket-android/build-android.sh
```

The APK is written to `dist/Badanaut-Android.apk`. Native builds use the pinned
Unicorn 2.1.4 submodule and enable ARM32 guests for both Android host ABIs.
See [native dependency versions](third_party/README.md).

## Source map and attribution

- `frontends/shared-ui/` — shared native layout, text and fonts.
- `frontends/pocket-desktop/` — Python/Qt desktop host and packaging.
- `frontends/pocket-android/` — Android host, package management and SDL activity.
- `third_party/bada-hle/` — Bada HLE engine and upstream notice.
- `cmake/Dependencies.cmake` — shared native dependency configuration.
- `third_party/` — pinned upstream library submodules and dependency notices.
- `.github/workflows/` — Windows, Linux and Android builds and validation.

The new app picker is an original adaptation of HyperHLE-Reborn's visual approach;
no code or graphics from that picker are copied. The Bada HLE source is published
here with upstream permission, as communicated to the maintainer; see
[UPSTREAM.md](third_party/bada-hle/UPSTREAM.md). The root PocketHLE license does
not apply to that component. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
