# Badanaut — Samsung bada emulator

Badanaut runs original 32-bit ARM Samsung bada applications through high-level
emulation. The Bada engine is based on [mariotaku/bada-hle](https://github.com/mariotaku/bada-hle).

The native launcher on **Windows, Linux, and Android** follows the compact
[touchHLE / HyperHLE-Reborn app picker](https://github.com/KlugKlugTG/HyperHLE-Reborn):
a black background, four columns of rounded game icons, gray labels, an arrow
for paging, and plain white buttons. Qt draws the desktop interface; Android
uses Canvas. Both use the same layout and bundled fonts. The browser UI has
been removed. Package artwork is detected where available; game details let
you select your own icon.

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

1. Choose **Open file manager** and select a Bada ZIP containing one `Bin/*.exe` ARM32
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

The application and games start **windowed on Windows and Linux**. Game windows
are resizable; **F11** toggles fullscreen. **Settings** offers:

- Original automatic output, 240×400, 480×800, 540×960, 720×1280 and 1080×1920.
- Fit or whole-pixel scaling, preserving aspect ratio.
- Sharp or smooth Canvas filtering and vertical sync.
- Optional fullscreen games.

Resolution follows each game's orientation. On desktop it sets the window size,
constrained to the monitor; on Android it limits the presentation area in physical
pixels inside the device screen. The guest always retains its original dimensions.
Whole-pixel scaling falls back to fitting when the screen is smaller than the
guest. Filtering applies to Canvas output; the game controls its own GL textures.
Vsync depends on driver support. Changes apply at the next game launch.

## Build

**Desktop:** Python 3.12, CMake 3.22+, SDL2, SDL2_image, OpenGL and a C/C++17
compiler. Windows uses Visual Studio and vcpkg. See
[desktop build instructions](frontends/pocket-desktop/README.md).

**Android:** JDK 17, Android SDK Platform/Build Tools 34, NDK `26.1.10909125`
and CMake `3.22.1`. Set `ANDROID_SDK_ROOT`, then run:

```sh
./frontends/pocket-android/build-android.sh
```

The APK is written to `dist/Badanaut-Android.apk`. Native builds download the
pinned Unicorn 2.1.4 source archive and verify its SHA-256. Rust is not required
for either Bada frontend.

## Source map and attribution

- `frontends/shared-ui/` — shared native layout, text and fonts.
- `frontends/pocket-desktop/` — Python/Qt desktop host and packaging.
- `frontends/pocket-android/` — Android host, package management and SDL activity.
- `third_party/bada-hle/` — Bada HLE engine and upstream notice.
- `third_party/unicorn/` — pinned ARM CPU build.
- `.github/workflows/` — Windows, Linux and Android builds and validation.
- `crates/` and `frontends/pocket-cli/` — retained legacy PocketHLE headless code;
  it is not part of the new Bada launchers.

The new app picker is an original adaptation of HyperHLE-Reborn's visual approach;
no code or graphics from that picker are copied. The Bada HLE source is published
here with upstream permission, as communicated to the maintainer; see
[UPSTREAM.md](third_party/bada-hle/UPSTREAM.md). The root PocketHLE license does
not apply to that component. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
