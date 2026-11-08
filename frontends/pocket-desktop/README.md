# Badanaut desktop

The Windows and Linux frontend uses the same offline HTML/CSS/JavaScript app
picker as Android. PySide6 hosts the picker, handles local file selection, and
starts the native Bada SDL/OpenGL engine in a separate process. Both the library
and games start windowed. Game windows can be resized; F11 toggles fullscreen.

## Build and run

Use Python 3.12, CMake 3.22+, a C/C++17 compiler, SDL2, SDL2_image and OpenGL.
On Ubuntu 22.04 or later:

```sh
sudo apt-get install cmake ninja-build libsdl2-dev libsdl2-image-dev libgl-dev libegl1 libopengl0 libxcb-cursor0 libxkbcommon-x11-0
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r frontends/pocket-desktop/requirements.txt
cmake -S . -B .build/desktop -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build/desktop --parallel 2
python frontends/pocket-desktop/main.py
```

On Windows, use a Visual Studio developer shell and install `sdl2:x64-windows`
and `sdl2-image:x64-windows` with vcpkg. Configure with
`-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` in addition to
the options above. Add `<vcpkg>/installed/x64-windows/bin` to PATH before running
or packaging. Activate the virtual environment with `.venv\Scripts\Activate.ps1`.

Unicorn 2.1.4 is downloaded at configure time using a pinned commit and verified
SHA-256. Rust is not required to build the new Bada frontends.

Run `python frontends/pocket-desktop/package.py` to bundle the application, shared
UI, engine, CPU library, Qt WebEngine and native dependencies into `dist/Badanaut/`.
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
`mainmenuicon` images in Res, Info, or the package root. Unsupported/missing
artwork gets a colored initials icon. The game details menu can replace it with
a local PNG/JPEG/WebP. Icons share rounded corners, a gloss option, shadows,
consistent spacing and labels. The design is an original implementation inspired
by [touchHLE / HyperHLE-Reborn](https://github.com/KlugKlugTG/HyperHLE-Reborn);
no source or assets from that project are bundled.

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
xvfb-run -a python frontends/pocket-desktop/main.py --library .build/smoke-library --smoke-test
```

The smoke check uses the real Qt bridge and shared UI. GitHub Actions also runs
it against the packaged Windows and Linux applications.
