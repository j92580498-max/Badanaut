# Third-party notices

## PocketHLE

The PocketHLE source in this repository retains the upstream dual license, Apache-2.0 OR MIT. See `LICENSE-APACHE` and `LICENSE-MIT`.

## Bada HLE

`third_party/bada-hle/` is based on [`mariotaku/bada-hle`](https://github.com/mariotaku/bada-hle). The upstream repository has no license file. The source is included here with the upstream author's permission to publish in Badanaut, as communicated to the Badanaut maintainer. This permission is specific to publication in this repository; it does not relicense the code or grant a general right to reuse it. See `third_party/bada-hle/UPSTREAM.md`.

## SDL2

SDL2 is under the zlib license. See `third_party/SDL2/LICENSE.txt`.

## Unicorn Engine

Unicorn and its bundled components retain their respective upstream terms. See `third_party/unicorn/COPYING`, `third_party/unicorn/COPYING.LGPL2`, and `third_party/unicorn/COPYING_GLIB`.

## pugixml

pugixml is distributed under the MIT license. See `third_party/pugixml/LICENSE.md`.

## PowerVR texture decoder

The unmodified decoder in `third_party/pvr/` comes from the PowerVR Native SDK,
copyright (c) Imagination Technologies Ltd., under the MIT license. See
`third_party/pvr/LICENSE.md` and `third_party/pvr/UPSTREAM.md`.

The Bada game archive and firmware libraries are not included in this repository.

## TLSF guest heap allocator

The unmodified TLSF allocator in `third_party/tlsf/` is copyright (c) 2006-2016
Matthew Conte, under the BSD 3-Clause license. See `third_party/tlsf/LICENSE.md`
and `third_party/tlsf/UPSTREAM.md`.

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
