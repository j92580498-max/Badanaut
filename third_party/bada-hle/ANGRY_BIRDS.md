# Angry Birds 1.6.3

The unmodified bada executable (`AppId bwvtftyy07`) declares API 2.0 and uses
the hard-float/VFP calling convention. On Windows it reaches the title screen,
episode and level selection, and playable levels with touch input and sound.
The user played through levels and confirmed smooth gameplay and normal bird
and pig idle animations after the random-number fix. The first outro completed
and the user continued to level 2-2.

## Runtime and package requirements

API version and floating-point ABI are separate properties. The supplied
Exploration 1.3.1 also declares API 2.0, but uses soft-float; Plants vs. Zombies
1.0.6 declares API 1.2 and uses hard-float. `ManifestVersion` describes the ZIP
metadata schema and must not be used as the OS/API version.

Angry Birds uses C++ exceptions during initialization. It needs the ARM
`libgcc_s.so.1` unwinder as well as a compatible `libstdc++.so.6`. The bundled
bada 2.0 SDK GNU runtime also needs `libsupc++.so` for its exception ABI.
All three are included in `bada_libs/hard-float/` and selected automatically.

To reproduce or inspect the bundled runtime from a local SDK ZIP:

```powershell
python tools/prepare-bada-runtime.py "path/to/bada_SDK_2.0.6_Common.zip"
```

This writes separate `soft` and `hard` sets under `.build/bada-sdk-runtimes/`.
On Windows the helper uses the SDK's bundled ARM linker. Other hosts can pass
`--linker /path/to/arm-bada-eabi-ld` with a host ARM GNU linker.

Manual imports of `libstdc++.so.6`, `libgcc_s.so.1` and `libsupc++.so` remain
available in the desktop and Android selectors. Imported versions and the legacy
single-file C++ runtime remain available. Launch selects the most recently
imported library with a matching calling convention, then the bundled default
for each component. See [GNU notices and provenance](../../bada_libs/README.md).

To launch the native engine directly from the repository root:

```powershell
.build/desktop/native/bada-hle.exe --input "path/to/AngryBirds"
```

The package must include its font resources, including
`Res/data/fonts/864x480_bada/FONT_BASIC.dat` and
`FONT_BASIC_N900_UNLOCALIZED.pvr`. One tested archive omitted these resources;
an otherwise identical complete archive contained them and booted with the
same executable. No game code or resources were modified.

## Engine changes

- The ELF loader reads `.ARM.attributes` instead of assuming the SDK version
  determines the float ABI. It also maps each image's ARM exception index and
  uses `DT_HASH` to count dynamic symbols correctly.
- The guest's real libgcc unwinder runs against those indexes through
  `__gnu_Unwind_Find_exidx`; C++ exception personality functions execute in the
  guest runtime rather than returning from an HLE no-op.
- Resource-index parsing uses working `strtok`, `strspn`, `strcspn` and
  `strpbrk` handlers. Tokenizer state belongs to the guest CPU.
- `printf` exposes game diagnostics, `abort` stops with an error, and
  `Application::Terminate` requests a clean exit from the host loop.
- `rand`/`srand` match the bada SDK's newlib 1.19 generator: a 64-bit state,
  multiplier 6364136223846793005, increment 1, and a nonnegative 31-bit result.
  Each guest owns its seed. The Windows CRT returns only 15 bits; Angry Birds
  divides that result by 2^31, making random idle delays nearly zero. This fix
  restores the expected range without changing the frame loop or system ticks.

## Outro investigation

The first outro uses the game's Lua sprite animation and countdowns. This
executable does not import `Osp::Media::Player`; its transition does not wait
for `OnPlayerEndOfClip`.

A captured timer trace contained 5,539 starts with a median interval of 17 ms.
The two long gaps (9.85 and 78.94 seconds) were bracketed by timer cancellation
on `OnBackground` and a restart on `OnForeground`. The game's own page
preparation also resets this outro's animation and countdowns on resume. The
outro subsequently completed without a timer or media-callback change.

A separate earlier run exited after a Lua exception at `gamelogic.lua:11487`,
where level selection indexes `page.bgColors[page.currentPage]`. The missing
entry suggests an invalid page index, but its cause has not been established.
Replay and resume transitions still need further verification; no game-script
patch or speculative page-index correction is included.

## Validation

Native regression probes cover ARM ABI metadata, relocated exception indexes,
runtime ABI mismatches, tokenization, the SDK's exact random sequence, unsigned
seeds, reseeding, independent guests and the game's random normalization.
Desktop and Android JVM tests cover package API metadata, retained runtime
versions and selection of compatible C++, unwinder and exception libraries,
including executables with no section table.

The actual three game packages passed desktop runtime-catalog selection with
both SDK multilib sets installed and the older C++ library preserved.
Exploration and PvZ passed short native smoke runs with the newer compatible
libraries. All three also passed 30-frame native launches with no runtime
arguments or runtime environment variables, using the adjacent bundle.
The explicit `--libstdcxx` override passed separately.
Angry Birds reached playable levels on both the older hard-float
C++ runtime plus libgcc and the complete SDK 2.0 hard-float set. Sixteen calls
to its real `RandomUtil::random` produced normalized values from approximately
0.115 to 0.982; the previous Windows implementation could not exceed 0.000016.

The Windows Release build passed all three native regression tests and all
28 desktop tests. Its packaged UI and both runtime sets were verified.
The complete Android APK built for ARM64 and ARMv7, passed all 36 Android JVM
tests, and passed runtime asset, architecture, application ID and signing
checks. The APK verifier's 12 tests passed separately. Windows builds of the
Android sources need the configure tools described in the Android README.

Gameplay was verified on Windows. Android device gameplay and a Linux build
have not been verified locally for this change; the PR workflows run native
regression and package checks on Linux and Windows. Games, SDK archives,
logs and saves remain local inputs/outputs. The six GNU
runtime files and their license notices are the documented exception.
