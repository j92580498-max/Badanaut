# Shared app picker

`index.html`, `style.css`, and `app.js` are the entire Badanaut library and settings
interface. Android packages this directory through its Gradle assets source set;
desktop packages exactly the same files with PyInstaller. No CDN or network is
used. Android loads a fixed intercepted HTTPS origin; Qt loads the local entry
point and connects through QWebChannel. Images arrive as bounded data URLs.

The host receives `{id, action, args}` and replies with
`{id, ok, result, error}`. Actions: `bootstrap`, `importGame`, `importRuntime`,
`setIcon`, `launch`, `remove`, `saveSettings`, and `diagnostics`. Native file
selection returns `false` when canceled. `bootstrap` supplies installed games,
runtime status, display settings and platform. Rendering never treats package
names or metadata as HTML.

The picker draws an evenly spaced, paged grid with classic glossy rounded icons,
name labels, search, sorting, details and a bottom dock. Touch swipes, mouse and
keyboard navigation work on the same controls. Settings have the same schema in
Python and Kotlin. On Android a selected resolution limits physical presentation
pixels inside the device surface; on desktop it sets the game window size.

Visual reference: [HyperHLE-Reborn app picker](https://github.com/KlugKlugTG/HyperHLE-Reborn/blob/trunk/src/environment/app_picker.rs),
derived from touchHLE. This is newly written UI code; no upstream artwork or
Objective-C/Rust picker implementation is copied.
