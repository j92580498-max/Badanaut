# Badanaut desktop

The Windows and Linux frontend paints the compact native game picker with
Qt/QPainter. Android paints the same layout with Canvas. The layout, strings,
and Liberation Sans fonts come from `../shared-ui/`; no browser is required.
The library opens in a decorated, resizable 320×480 window. Enlarging it scales
and centers the reference layout, keeping four columns and 16 games per page.
Horizontal swipes move between pages, with dots showing the current page.
The top-right ⋮ menu contains Open file manager, Copyright info and Settings.

PySide6 handles local file selection and starts the Bada SDL/OpenGL engine in a
separate windowed process. Game windows can be resized; F11 toggles fullscreen.

## Build and run

Use Python 3.12, CMake 3.22+, a C/C++17 compiler and OpenGL. SDL2, SDL2_image
and most other native libraries build from pinned submodules. The small PowerVR
decoder is included as vendored source.
On Ubuntu 22.04 or later:

```sh
git submodule update --init --recursive --depth 1
sudo apt-get install cmake ninja-build libgl-dev libegl1 libopengl0 libasound2-dev libpulse-dev libudev-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxinerama-dev libxxf86vm-dev libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 libxcb-xinerama0 libxcb-shape0 libxcb-shm0 libxcb-sync1 libxcb-randr0 libxcb-render0 libxcb-xfixes0 libxcb-xkb1 libx11-xcb1 libxkbcommon-x11-0
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r frontends/pocket-desktop/requirements.txt
cmake -S . -B .build/desktop -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build/desktop --parallel 2
python frontends/pocket-desktop/main.py
```

On Windows with Visual Studio 2022:

```powershell
git submodule update --init --recursive --depth 1
py -3.12 -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -r frontends/pocket-desktop/requirements.txt
cmake -S . -B .build/desktop -G "Visual Studio 17 2022" -A x64
cmake --build .build/desktop --config Release --parallel 2
python frontends/pocket-desktop/main.py
```

The same pinned Unicorn 2.1.4, SDL2 and support libraries are used by desktop
and Android. See [native dependencies](../../third_party/README.md).

Run `python frontends/pocket-desktop/package.py` to bundle the application, shared
layout/fonts, engine, CPU library, Qt Essentials and native dependencies into `dist/Badanaut/`.
The script also creates a Windows ZIP or Linux tar.gz. Extract the complete
archive and run `Badanaut.exe` or `./Badanaut/Badanaut`; keep its `_internal`
directory. Neither Python nor an Android SDK is needed to run a packaged build.
Linux packages are built on Ubuntu 22.04 and require a compatible glibc and an
OpenGL driver. The CI artifacts target x86-64.

## Library and display

Import Bada ZIP packages with `Bin/*.exe` and `Info/Appinfo.sys` or
`Info/manifest.xml`. A single containing directory is supported. A Bada ARM32
`libstdc++.so.6` from the SDK/firmware must be selected separately; its float ABI
must match the game. Packages and firmware are never included in distribution.

Artwork comes from the metadata's `Icon` path, or `icon`, `appicon`, or
`mainmenuicon` images, or an executable-named image such as `Res/SkyForce.png`. Unsupported/missing
artwork gets a charcoal tile with white initials. Right-click or hold an icon
for details and select a local PNG/JPEG/WebP as artwork. Icons are center-cropped
and rounded without a gloss overlay. Use a horizontal swipe, trackpad horizontal scrolling or
Page Up/Down to change pages. Arrow keys/Tab select a control; Enter opens it,
Menu opens game details, and Escape goes back.

The layout follows the reference from
[touchHLE / HyperHLE-Reborn](https://github.com/KlugKlugTG/HyperHLE-Reborn).
No source or game artwork from that project is bundled.

Settings apply on the next launch. Resolution controls the presentation window,
with orientation following the game and large windows constrained to the monitor.
The guest keeps its original dimensions. Whole-pixel scaling keeps integer
multiples where the display is large enough, falling back to fitting on smaller
displays. Filtering affects Canvas output, leaving the game's own GL texture
choices intact. Vsync depends on driver support. Fullscreen is optional.

The library uses the OS application-data directory (`Badanaut/bada`), with
`games/<id>/.badanaut-game.json`, `display.json`, and a separate runtime file.
`--library <directory>` and `--engine <file>` override the defaults for development.
Launch logs are bounded to 2 MB and can be exported in Settings.

## Validation

```sh
python -m unittest discover -s frontends/pocket-desktop/tests -v
xvfb-run -a python tools/check-shared-ui.py --screenshots .build/ui-preview
xvfb-run -a python frontends/pocket-desktop/main.py --library .build/smoke-library --smoke-test
```

The smoke check uses the native renderer and real library host. GitHub Actions also runs
it against the packaged Windows and Linux applications.
