# Bundled Bada GCC runtimes

These ARM32 guest libraries come from the GNU GCC 4.5.3 toolchain in Samsung
bada SDK 2.0.6 Common. They execute inside the emulator, on Windows, Linux and
Android; they are not host DLLs or Android JNI libraries.

Both `soft-float/` and `hard-float/` contain `libstdc++.so.6`, `libgcc_s.so.1`
and `libsupc++.so`. The engine selects the game's calling convention from
ARM ELF flags and attributes. API 1.2 versus 2.0 does not determine that ABI.
The C++ runtime is required; the companions provide exception support.

The GNU libraries retain GPLv3-or-later with the GCC Runtime Library Exception
3.1. `COPYING.libstdcxx` covers libstdc++ and libsupc++; `COPYING.libgcc` covers
libgcc. Both contain the GPL and exception texts. `COPYING.RUNTIME` also
retains the exception separately. These terms do not relicense Badanaut, its
HLE engine, Samsung firmware or game data. The
[corresponding GNU source download and checksums](SOURCE.md) accompany this
bundle; no full SDK download is needed to run games.

`manifest.json` and `SHA256SUMS` identify the exact files. To check the bundle:

```sh
python tools/verify-bada-libs.py
```

To reproduce the libraries from the same SDK ZIP, run
`tools/prepare-bada-runtime.py`, then compare the six outputs against
`SHA256SUMS`. The SDK's `libstdc++.so.6` and `libgcc_s.so.1` are copied
unchanged. `libsupc++.so` is linked from the SDK's `libsupc++-xnew.a`, using its
GNU linker with `-shared -soname libsupc++.so --whole-archive`.

CMake copies this directory beside the desktop engine. Desktop releases and
Android APK assets include the same files and notices. Manual runtime imports
and native `--libstdcxx`, `--libgcc` and `--libsupcxx` overrides remain available.
