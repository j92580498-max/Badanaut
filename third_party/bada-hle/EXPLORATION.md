# Exploration compatibility

Verified with the unmodified ARM32 `Bin/Exploration.exe`, AppId `559t46hurs`.
The rendered menu identifies Exploration 1.3.1 by Andrzej Chomiak. The executable
SHA-256 is `d36390ab33ec99f68fa92e320510ecabb321515315f05a4e2dc3dd5deb911d6d`.
This build uses soft-float and GLES 1.x. The user-supplied ARM `libstdc++.so.6`
is mapped as the guest C++ runtime. Game files, firmware, saves, screenshots
and diagnostic dumps remain local inputs and outputs.

## Startup and guest callbacks

Exploration opens its locale and configuration files with `fopen(..., "rt")`.
The previous HLE appended `b`, producing `rtb`, which aborts the Windows CRT.
Both stdio and Osp File modes are now validated and normalized to binary host
streams. This preserves newlib's byte semantics, including CRLF and Ctrl-Z.

Adding the application's form must invoke `Control::OnInitializing` before
attachment. The frame's virtual AddControl slot and the direct Container import
now enter that callback in the existing Unicorn execution slice, preserve the
caller context and return the initialization result. Successful initialization
attaches once; failure permits retry. Nested child initialization and ARM/Thumb
interworking are supported. Form orientation and Rectangle value-return bounds
provide the landscape dimensions used by the game.

The missing Thumb switch-table helpers now preserve the selector and select the
proper inline byte or halfword branch offset. `strchr` and `strrchr` can find a
string's NUL terminator, and `__aeabi_f2uiz` returns an unsigned core-register
result with defined saturation. Debug heap frees and the locale language-string
query also have handlers.

## Files and saved worlds

Guest descriptor flags use newlib values: `0x200` for create, `0x400` for truncate,
`0x800` for exclusive creation and `8` for append. Reopening a writable save
preserves its contents; read-only and write-only access are enforced. Journal
unlink removes writable files and retains packaged resources.

Bada's `DirEnumerator::GetCurrentDirEntry` returns a reference in r0, unlike the
later Tizen API's value return. The direct import now returns the enumerated
entry, including its name, size, directory/read-only/hidden flags and modification
date. Reset and end-of-enumeration invalidate the current entry.

DateTime fields use the guest object layout, including packed seconds and
milliseconds. SystemTime populates those fields; GetTime returns their time of
day rather than reading an unrelated host clock. `mktime` and `strftime` marshal
newlib's nine-field tm explicitly, allowing saved-world dates to be formatted.

## Rendering and Windows CPU stability

Exploration packs terrain positions and texture coordinates as signed bytes.
Desktop compatibility GL rejects these array types, so the desktop HLE converts
their numeric values to floats while retaining the guest descriptors. Client
arrays, interleaved VBO offsets and indexed draws are covered. Android GLES
retains its native array types. GLES 1 readback, stencil clear and texture-env
vector imports are registered as well.

Longer Windows runs exposed an access violation in the pinned Unicorn library.
Its JIT page-commit handler could request memory beyond its reservation, and its
x64 exception trampoline reserved insufficient shadow space with incorrect
stack alignment. `cmake/UnicornWindows.cmake` corrects a generated build copy of
the source. The upstream submodule, pin and original notices remain intact.
The standalone Windows cache probe crashes with the original library and passes
with the correction, exercising 1 MiB and 8 MiB caches, growth and recycling.

## Validation

The Windows VS2022 Release build completed. Exploration ran 7,500 frames and
exited with code 0. Menus, new terrain, camera/jump input and a saved-world
selection/reload were exercised; the reloaded world renders grass and trees.

Eight targeted checks passed: framework/ABI boundaries, file modes and metadata,
workers, touch, UI, signed-byte/fixed-point rendering with both guest float ABIs,
and the Windows JIT cache probe. The framework probe also checks date formatting,
buffer limits, initialization retry and nested ARM/Thumb callbacks.

The broader native test harness has seven Windows failures: two SDL disk-audio
probes, the ABI environment-toggle probe and four Canvas pixel-readback probes.
All seven reproduce with the same messages against unchanged commit `609ac18`;
they are excluded from the passing count. This investigation did not run Linux
or Android builds or establish Android hardware performance. Validation covers
the supplied 1.3.1 executable rather than other archived versions.
