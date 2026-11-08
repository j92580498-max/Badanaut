# Shared native picker assets

`launcher.json` defines the 320×480 reference canvas, four-column grid, 58-pixel
rounded icons, gray labels, a top-right ⋮ drop-down menu and page dots. It also contains
display choices, text and colors. `fonts/` contains unmodified Liberation Sans
2.1.5 regular/bold fonts and their SIL OFL 1.1 license. These assets are packaged
unchanged in Windows, Linux and Android builds.

Desktop draws with `pocket-desktop/launcher.py` (Qt/QPainter); Android draws with
`LauncherView.kt` (Canvas). The reference canvas scales uniformly and centers in
the available client area; it always has four columns and up to 16 games.
Horizontal swipes move between pages without wrapping at the ends. Page dots
appear when multiple pages are needed. Icons are center-cropped into rounded rectangles without added gloss.
Long labels are centered and clipped to their cell, following the reference.
Missing artwork uses a restrained charcoal tile with white initials.

The native host receives `{id, action, args}` and replies with
`{id, ok, result, error}`. Actions: `bootstrap`, `importGame`, `importRuntime`,
`setIcon`, `launch`, `remove`, `saveSettings`, and `diagnostics`. File selection
returns `false` when canceled. `bootstrap` supplies installed games, runtime
status, display settings and platform. Package names are drawn as plain text.

Visual reference: [HyperHLE-Reborn app picker](https://github.com/KlugKlugTG/HyperHLE-Reborn/blob/trunk/src/environment/app_picker.rs),
derived from touchHLE, and the user's supplied screenshot. The ⋮ menu
opens the file picker, copyright info or Badanaut settings. No upstream code or game artwork is bundled.

Run `xvfb-run -a python tools/check-shared-ui.py --screenshots .build/ui-preview`
with desktop requirements installed to check reference geometry, artwork,
paging, scaled click targets, long press, details, persisted settings and errors.
