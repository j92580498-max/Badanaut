# Third-party notices

## Project licensing

The launcher code and supporting scripts retain this repository's dual license,
Apache-2.0 OR MIT. The original PocketHLE attribution remains in `LICENSE` and
`LICENSE-MIT`. These licenses do not relicense the Bada engine or its dependencies.

## Bada HLE

`third_party/bada-hle/` is based on [`mariotaku/bada-hle`](https://github.com/mariotaku/bada-hle). The upstream repository has no license file. The source is included here with the upstream author's permission to publish in Badanaut, as communicated to the Badanaut maintainer. This permission is specific to publication in this repository; it does not relicense the code or grant a general right to reuse it. See `third_party/bada-hle/UPSTREAM.md`.

## SDL2

SDL2 is under the zlib license. See `third_party/SDL2/LICENSE.txt`.

## SDL2_image

SDL2_image is under the zlib license. See `third_party/SDL2_image/LICENSE.txt`.
The desktop build uses its bundled stb PNG/JPEG codecs and screenshot encoders.

## cxxopts

cxxopts is distributed under the MIT license. See `third_party/cxxopts/LICENSE`.

## Unicorn Engine

Unicorn and its bundled components retain their respective upstream terms. See `third_party/unicorn/COPYING`, `third_party/unicorn/COPYING.LGPL2`, and `third_party/unicorn/COPYING_GLIB`.

Badanaut's Windows x64 build applies the JIT allocation fixes in `cmake/UnicornWindows.cmake` to a generated copy of the upstream source. Its original notices and license terms are retained.

## pugixml

pugixml is distributed under the MIT license. See `third_party/pugixml/LICENSE.md`.

## PowerVR texture decoder

The unmodified decoder in `third_party/pvr/` comes from the PowerVR Native SDK,
copyright (c) Imagination Technologies Ltd., under the MIT license. See
`third_party/pvr/LICENSE.md`. The upstream revision and original decoder path
are documented in `third_party/pvr/UPSTREAM.md`.

The Bada game archive and firmware libraries are not included in this repository.

## TLSF guest heap allocator

The unmodified TLSF allocator in `third_party/tlsf/` is copyright (c) 2006-2016
Matthew Conte, under the BSD 3-Clause license retained in `third_party/tlsf/tlsf.h`.
The pinned revision is documented in `third_party/README.md`.

## Nuklear

Nuklear and its SDL/OpenGL 2 backend are available under the MIT license or
public domain terms. See `third_party/nuklear/LICENSE`. The single header
`third_party/nuklear/nuklear.h` also retains the notices for its embedded stb
code and ProggyClean font. Both files are included in distribution notices.

## MP3 and WAV audio decoders

The unmodified `dr_mp3.h` and `dr_wav.h` decoders from
[dr_libs](https://github.com/mackron/dr_libs) are available under public domain
or MIT No Attribution terms, copyright 2020 David Reid. The pinned revision is
documented in `third_party/README.md`; see `third_party/dr_libs/LICENSE` and the
notices retained in each header. The license is included in desktop and Android
distribution notices.

## New desktop UI dependencies

The desktop application uses dynamically linked Qt/PySide6 through
[PySide6 Essentials 6.8.3](https://doc.qt.io/qtforpython-6/licenses.html).
Qt/PySide6 carry LGPL/GPL/commercial licensing options and third-party
notices; retain the notices and license files from
the redistributed wheels. The bundled Python host is frozen with
[PyInstaller](https://pyinstaller.org/en/stable/license.html), whose license
includes the bootloader exception permitting application distribution.

The shared Badanaut app picker is newly written code inspired visually by
[HyperHLE-Reborn](https://github.com/KlugKlugTG/HyperHLE-Reborn), a fork of
HyperHLE/touchHLE. No source, logo, wallpaper, or app artwork from those projects
is copied into Badanaut. Game icons are read from user-supplied packages or images.

The native launcher bundles unmodified Liberation Sans Regular and Bold 2.1.5,
licensed under the SIL Open Font License 1.1. Copyright 2010 Google Corporation,
2012 Red Hat, Inc. See `frontends/shared-ui/fonts/LICENSE.txt`.
