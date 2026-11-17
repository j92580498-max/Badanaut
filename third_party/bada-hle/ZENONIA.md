# Zenonia compatibility

Investigated with the original `Bin/Zenonia.exe` from the supplied
[Zenonia archive](https://archive.org/download/badaappsgamescollection/Zenonia.zip).
The package uses AppId `u4cdw287po`, bada API 1.0 and the ARM32 hard-float ABI.
The executable and graphics resources were not patched. Game files, SDK
references, build outputs and captures remain local validation inputs/outputs.

| Input | SHA-256 |
|---|---|
| `Zenonia.zip` | `de30b74a52db7776e881a95005dad577a5f2dc19130623b7045eb192a96e3a49` |
| `Bin/Zenonia.exe` | `a5b89352fc8165dc1d2016608d87d5d96202fe572de3bc62ef3609700911b7a1` |

## Startup and resource handles

The original emulator faults during `OnAppInitializing` when the game destroys
a stack array of framework Strings through virtual slot zero. Strings now have
shared, callable destructor tables and release their owned UTF-16 storage.
Capacity construction, wide-string assignment and `operator+=` also work;
assignment and append operators return the String reference. Resource paths
therefore retain their `/Res/320x240/` prefix.

A second failure came from the guest address map. `MC_knlGetResourceID` returns
an allocated `File*` as a signed integer, and `GcxLoadResData` requires it to be
positive. Heap pointers beginning at `0x80000000` failed this check even though
the files opened successfully. The 512 MiB heap now begins at `0x48000000`,
below the signed-address boundary and clear of the HLE pages and guest stacks.
The existing Fruit Ninja texture diagnostic uses the heap mapping to validate
pointers instead of depending on the previous heap address.

## Canvas rendering

Zenonia draws its game scene into a 320x240 RGB565 Bitmap. It rotates this image
90 degrees around `(0,0)`, placing the pivot at `(0,319)` in an independent
240x320 memory Canvas. A scaled Canvas copy produces the 480x800 screen image,
then rotated native UI bitmaps are drawn over it and `Show` presents the frame.

Canvas and Bitmap instances now have separate pixel storage and virtual
destructors. `IAppFrame::GetCanvasN` returns caller-owned wrappers sharing the
screen buffer. Lock descriptors report the width, height, row pitch, bit depth,
pixel format and guest pixel pointer. Image decoding uses the existing pinned
SDL2_image dependency, including scaled decoding for the splash.

The rendering implementation supports the copy, clipping, rotation, masking
and alpha composition used by the game. Positive rotation is counterclockwise
in screen coordinates. The destination point locates the rotated source pivot;
the game's negative pivots position native UI buttons in the same rotated
coordinate system. Adding the pivot to the destination a second time displaced
the menu buttons, despite the background rendering correctly.

The portrait guest buffer contains a rotated landscape scene. Its dimensions
and the game's own touch-coordinate transform remain intact.

## Buffers and save operations

ByteBuffer descriptors belong to the guest CPU and retain independent positions
and limits when they share storage. Destruction releases storage after the last
descriptor is gone. `SetArray` and `GetArray` apply the supplied index to the
external array, transfer from the buffer's current position and advance it.
Indexed byte writes, relative integer writes and bounds checks are implemented.

`File::Read(ByteBuffer&)` fills from the current position and records the filled
limit. `File::Write(const ByteBuffer&)` writes position through limit while
preserving the const descriptor. `File::Move` performs the save-file rename
and rejects an existing writable destination. The older UTC SystemTime overload
and TimeSpan ticks accessor are implemented; SystemTime ticks use milliseconds
since the Unix epoch, as documented by bada 1.0.

## Audio settings

Zenonia's `MC_fsRead` masks failed framework results with `0xffff` before
checking them. The generic `0x80000000` returned for an unopened file therefore
became success. A missing `option.sav` appeared to contain a valid zero-filled
payload, replacing the default volume and music settings with zero.

File construction and ByteBuffer reads now return the documented bada errors
with their cause bits intact, including `E_FILE_NOT_FOUND`, `E_INVALID_STATE`,
`E_INVALID_ARG`, `E_END_OF_FILE` and host I/O errors. Reads validate the buffer
before calculating its remaining space. Missing and empty options files can
no longer masquerade as successful reads. Raw reads retain their byte-count
return convention, and successful reads and const buffer writes retain their
existing behavior.

A fresh private-data directory reproduced volume zero before the fix and the
game's default volume of 25 after it. The original startup and menu WAV effects
then opened through Player and produced nonzero stereo S16 PCM through SDL's
disk audio driver at 48,000 Hz. The supplied WAV files use mono 16-bit PCM at
22,050 Hz, which the existing decoder and mixer already support. Game binaries
and saved settings were not patched. Looping music and audible device playback
have not been separately validated.

## References and validation

Declarations were located with `gh search code`, including `FGrpCanvas`,
`FBaseByteBuffer`, `FBaseErrors`, `FSysSystemTime`, `FIoFile` and
`FIoFileAttributes`. Behavior was checked
against the original
[bada 1.0 API reference](https://github.com/romanm11/bada-SDK-1.0.0-Help/blob/master/com.osp.apireference.help_1.0.0.jar),
with the
[published successor headers](https://github.com/kitsilanosoftware/CppSharpTizen/tree/92e42a36cc3c5f2b1636061e82025feec4edda0d/tizen-sdk/platforms/tizen2.1/rootstraps/tizen-emulator-2.1.native/usr/include/osp)
as a supplementary reference. The game binary confirms its object field offsets
and the `IAppFrame` virtual canvas getter.

The unmodified executable boots past the Gamevil splash, displays its title
and main menu, reads all three packaged save slots and starts a new character.
The introductory scenes, dialogue and the first house scene render, and SDL
finger input selects menu items, confirms dialogs and opens character stats.

Validation used Linux aarch64, Xvfb, Mesa software OpenGL and dummy SDL audio.
Both native build configurations completed. All three desktop regression tests
passed; the native suite passed 20 tests and skipped its two native GLES1 context
probes before guest execution because that context is unavailable on this host.

New probes exercise independent and shared Canvas storage, RGB565 rotation with
zero and negative pivots, scaled and overlapping copies, masked/transparent UI
bitmaps, image decoding, virtual object destruction, String array cleanup and
ByteBuffer sharing/offsets. A file probe writes, moves and reopens a save payload
and verifies that its bytes and const buffer state survive the round trip.
The file regression also checks missing and empty settings files, the guest's
result mask, unchanged bytes after a failed raw read and invalid read buffers
and file modes. It failed against the previous engine and passes with the fix.

Android device/APK validation, audible playback and a complete campaign have
not been performed. The shared engine and Android GLES presenter are covered
by the native build/probes. Live in-game checkpoint persistence has not yet
been established by an emulator restart.
