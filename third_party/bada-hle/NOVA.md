# N.O.V.A. compatibility

These fixes were investigated with the original ARM32 N.O.V.A. executable
(`AppId 107qye4hl9`). Game packages and the Bada firmware runtime are local test
inputs.

## Causes

- Irrlicht deletes `Osp::Io::File` through virtual destructor slot 1. The old
  no-op vtable kept whole resource-pack copies alive. Files now own streams,
  transfer bounded chunks, and implement complete/deleting destruction.
  Identical HLE class vtables are shared within each CPU instance.
- BGRA8888 uploads were sized as one byte per pixel. The host driver read past
  the staging buffer, corrupting the title and fonts. BGRA uses four bytes;
  native ES uses the BGRA internal format required by its extension.
- Fixed-function state queries and separate texture-coordinate arrays for
  each client texture unit were missing. Queries now report guest pointers and
  bindings, and staging restores the client texture unit. All query types keep
  the guest viewport and allocate the full variable-length texture-format list.
- PVRTC texture uploads, including tiny mip levels, were dropped. The
  unmodified PowerVR decoder supplies RGBA uploads on hosts without PVRTC.
- `wcscmp` always returned equality. Nova's ELF selects two-byte `wchar_t`;
  comparing UTF-16 code units restores XML type selection.
- File write modes and removal were ignored, retaining stale checkpoints when
  starting New Game. Streams now support truncation, append, writes and removal.
- Guest `free` was a no-op. Temporary allocations accumulated in the 512 MB
  bump heap. The unmodified TLSF allocator now recycles and coalesces storage
  shared with Unicorn; `calloc` clears recycled bytes and `realloc` preserves
  the valid prefix. Immediate GL uploads and non-overlapping `memcpy` reads
  use the shared heap directly; overlapping moves retain their staging path.
  Requested alignment is applied in the guest address space when the host
  pool has a different alignment; frees retain the original TLSF block.
- Unicorn's built-in wall-clock guard creates a thread for every callback and
  polls every two microseconds. One reusable watchdog now waits for the
  deadline or cancellation, retains the 120-second initialization guard, and
  reports expiry as failure. It joins before the CPU is closed.

## Sources

The implementations and documentation were found with `gh search code` and
read before adapting the corresponding boundaries:

- touchHLE, commit `ca1de89c578e79746f33c1332e28efd106c334ba`:
  [stdio](https://github.com/touchHLE/touchHLE/blob/ca1de89c578e79746f33c1332e28efd106c334ba/src/libc/stdio.rs),
  [guest GL marshalling](https://github.com/touchHLE/touchHLE/blob/ca1de89c578e79746f33c1332e28efd106c334ba/src/frameworks/opengles/gles_guest.rs),
  [native ES BGRA handling](https://github.com/touchHLE/touchHLE/blob/ca1de89c578e79746f33c1332e28efd106c334ba/src/gles/gles1_native.rs),
  [texture-unit state](https://github.com/touchHLE/touchHLE/blob/ca1de89c578e79746f33c1332e28efd106c334ba/src/gles/gles1_on_gl2.rs),
  [PVRTC handling](https://github.com/touchHLE/touchHLE/blob/ca1de89c578e79746f33c1332e28efd106c334ba/src/gles/util.rs),
  [image decoding](https://github.com/touchHLE/touchHLE/blob/ca1de89c578e79746f33c1332e28efd106c334ba/src/image.rs),
  [wide strings](https://github.com/touchHLE/touchHLE/blob/ca1de89c578e79746f33c1332e28efd106c334ba/src/libc/wchar.rs),
  [generic character comparison](https://github.com/touchHLE/touchHLE/blob/ca1de89c578e79746f33c1332e28efd106c334ba/src/libc/generic_char.rs),
  [memory-copy entry points](https://github.com/touchHLE/touchHLE/blob/ca1de89c578e79746f33c1332e28efd106c334ba/src/libc/string.rs), and
  [direct guest-memory moves](https://github.com/touchHLE/touchHLE/blob/ca1de89c578e79746f33c1332e28efd106c334ba/src/mem.rs).
- [PowerVR decoder](../pvr/UPSTREAM.md), commit
  `63f0098acdc11ec9a072c30000d2fa670a955bee` (MIT, unmodified).
- [TLSF pool allocator](../tlsf/UPSTREAM.md), commit
  `deff9ab509341f264addbd3c8ada533678591905` (BSD 3-Clause, unmodified).
- [Itanium C++ ABI, virtual destructor entries](https://github.com/itanium-cxx-abi/cxx-abi/blob/fae5e46c86147a457a057c6372fa95ec38209fe8/abi.html).
- Khronos [glGetPointerv](https://github.com/KhronosGroup/OpenGL-Refpages/blob/4bdf67a73d040f79bd7b087f5c1e588809e1f329/es1.1/glGetPointerv.xml) and
  [state query sizes and conversions](https://github.com/KhronosGroup/OpenGL-Refpages/blob/4bdf67a73d040f79bd7b087f5c1e588809e1f329/es1.1/glGet.xml), and
  [BGRA8888 extension](https://github.com/KhronosGroup/OpenGL-Registry/blob/6af574a14089ccfee87efe230ebcdd8742859813/extensions/EXT/EXT_texture_format_BGRA8888.txt).
- [Unicorn's borrowed host-memory mapping API](https://github.com/unicorn-engine/unicorn/blob/8028ec436f2d9376525352dd38ed9ed6b9f6be10/include/unicorn/unicorn.h).
- [Unicorn timeout worker](https://github.com/unicorn-engine/unicorn/blob/8028ec436f2d9376525352dd38ed9ed6b9f6be10/uc.c),
  [SDL's cancellable timer-worker loop](https://github.com/libsdl-org/SDL/blob/822c2ae4bab3cbc41fb5707cd10abe7ca0280484/src/timer/SDL_timer.c), and
  [standard-library deadline/predicate waiting](https://github.com/microsoft/STL/blob/e1ed302683e5e822f663fbb0f44cee7e9ddc6af0/stl/inc/condition_variable).

## Verification and performance

Native regression probes live in `frontends/pocket-android/tests/native`.
They cover streamed reads, file lifetimes and checkpoints, BGRA drawing, PVRTC
decoding, fixed-function state, UTF-16 comparison, heap recycling and existing
audio/timer, ABI, pixel and display behavior. Native ES 1 checks return skip code
77 when the host cannot create the required context; desktop compatibility GL
provides additional coverage of the ES 1 guest handlers.

The final checks passed for desktop ES 1 handlers and native GLES 2, each with
hard- and soft-float guest calls. They include BGRA images and subimages,
PVRTC tiny mip levels and alpha, independent texture-unit arrays, complete
compressed-format query lists, and guest viewport values after resizing.
Heap, memory-copy, watchdog, UTF-16, file, audio/timer and ARM/Thumb ABI probes
passed before the final alignment correction. The existing pixel probe found
that a page-aligned TLSF host address did not imply a page-aligned guest
address. The allocator correction above has not been rebuilt or retested;
the remaining pixel, display and diagnostic-log checks are incomplete.

An unprofiled 800×480 gameplay run before the final alignment correction
reached the first-person tutorial. Joystick
input changed the player's position, firing produced visible shots, and the
rifle magazine decreased to 13 rounds. The runner observed a peak game process
RSS of about 167 MiB. Gameplay measured approximately **7–8.5 FPS**, with
roughly **114–140 ms of busy time per frame**, using llvmpipe and
`LP_NUM_THREADS=8`. **Stable 30 FPS remains unresolved.** These measurements
describe this Linux/proot software-rendering host; native Android GLES
performance has not been measured.

Use `BADA_TRACE_FPS=1` to measure actual frame throughput and busy time. Nova
re-arms a 5 ms timer; that interval is not a guarantee of rendered FPS. The ARM
guest executes on one thread. Native Android uses its GLES driver; headless
desktop tests use llvmpipe, whose rendering worker count can be set separately
with `LP_NUM_THREADS`. Mesa's [environment-variable documentation](https://github.com/marchaesen/vcxsrv/blob/d0a1eaf7ee15fcdf4f683388a88fec49078e6408/mesalib/docs/envvars.rst)
describes that option. Keep compilation and game runs separate when testing on
memory-constrained hosts.

Emulator captures for this investigation are saved directly in
`/root/emulator photo gallery`.
