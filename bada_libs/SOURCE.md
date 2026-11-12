# Corresponding source and provenance

## Download the corresponding GNU source

Samsung's **bada SDK 2.0.6 Source Code** distribution contains the corresponding
GNU toolchain source for these libraries, including Samsung's target changes,
patches and build scripts. Internet Archive preserves this release:

- [Download `bada-g++-4.5-3-src.tar.gz`](https://archive.org/download/bada_SDK_2.0.6_src/bada_SDK_2.0.6.zip/bada-g%2B%2B-4.5-3-src.tar.gz).
- [Browse the SDK 2.0.6 source distribution](https://archive.org/details/bada_SDK_2.0.6_src),
  or download `bada_SDK_2.0.6.zip` there and extract that same GNU source archive.
- [Alternative source mirror](https://archive.org/download/Bada_SDK_Source/bada_SDK_2.0.6.zip/bada-g%2B%2B-4.5-3-src.tar.gz)
  from the [Samsung Open Source release collection](https://archive.org/details/Bada_SDK_Source).

The GNU source archive is **274,733,874 bytes**. Its SHA256 is:

```text
6d028c8c5d907359dab639adb8b541eb67e4cd45b3f1c30f61c1d433c3894f2d
```

Extract it with `tar -xzf bada-g++-4.5-3-src.tar.gz`. The source includes:

- `45/src/gcc-4.5.3/`, whose `gcc/BASE-VER` identifies version 4.5.3.
- `45/src/gcc-4.5.3/libstdc++-v3/`, including the `libsupc++` exception ABI
  and Samsung's `libsupc++-xnew/` build configuration.
- GCC's ARM libgcc implementation and `gcc/config/arm/t-bada-eabi`, which
  defines the SDK's soft-float and hard-float/VFP multilibs.
- Samsung's patches, `45/build.sh`, `45/build-others.sh` and related toolchain
  sources, including `45/src/newlib-1.19.0/`.

These download directions accompany the object files in Git, desktop packages
and Android assets. GPLv3 section 6(d) permits corresponding source on a
separate server with clear directions alongside the binaries. Redistributors
must keep the source available for as long as required; if a mirror disappears,
preserve the archive on another server and update these links before further
distribution. The archive is hosted separately to keep emulator clones small.

## SDK binary provenance and local reproduction

The bundle was prepared from a user-supplied Lunastore mirror of
`bada_SDK_2.0.6_Common.zip`, using its GCC 4.5.3 `arm-bada-eabi` toolchain.
The archived Samsung download is
[bada SDK 2.0.6 Common](https://web.archive.org/web/20140810175413if_/http://static.bada.com/sdk-update/2.0.6/bada_SDK_2.0.6_Common.zip),
linked from the [archived Samsung SDK page](https://web.archive.org/web/20141205212353/http://developer.bada.com/devtools/sdk).
The local ZIP is 678,890,944 bytes, with SHA256:

```text
555bd2d6a2ff34ca8da9481f5e959146abdf40537b1e4c7bd0cd27a4f112904a
```

Its SHA1 `0d1dd5a6cfac4105d9193037b9cc3ba67a3018ff` matches the archived
Samsung download's CDX content fingerprint.

Soft-float inputs are under `Tools/Toolchains/ARM/arm-bada-eabi/lib/`;
hard-float inputs are under its `hard/armv6/vfp/` subdirectory. The SDK's C++
headers retain GPLv3-or-later and GCC Runtime Library Exception 3.1 notices.

`tools/prepare-bada-runtime.py` records the extraction and linking process.
`libstdc++.so.6` and `libgcc_s.so.1` are unchanged SDK files; `libsupc++.so`
is linked from GNU `libsupc++-xnew.a` with the SDK's linker. See `README.md`
and `SHA256SUMS` for reproduction and binary checks. The helper does not
distribute the full SDK, proprietary Osp libraries, firmware or games.

Unmodified upstream GCC 4.5.3 is also available from
[GNU](https://ftp.gnu.org/gnu/gcc/gcc-4.5.3/gcc-4.5.3.tar.bz2) and
[gcc-mirror](https://github.com/gcc-mirror/gcc/tree/releases/gcc-4.5.3).
Use Samsung's source release above for its target changes.
