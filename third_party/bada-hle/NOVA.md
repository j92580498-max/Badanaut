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
  Recycled allocation payloads are cleared, preserving the previous heap's
  initial values and following touchHLE's allocation behavior.
- Unicorn's built-in wall-clock guard creates a thread for every callback and
  polls every two microseconds. One reusable watchdog now waits for the
  deadline or cancellation, retains the 120-second initialization guard, and
  reports expiry as failure. It joins before the CPU is closed.

## Fruit Ninja and multi-touch follow-up

The initial TLSF integration changed ordinary allocations from cleared memory
to recycled payload bytes. Fruit Ninja's `CoinCounter` constructor leaves an
optional visibility field at offset `0x8c` at its initial value. Stale bytes
made `CoinCounter::Draw` enter a path that bound a null texture before the first
frame. The pre-PR build rendered normally; clearing the TLSF payload restored
the same initialization behavior, and Fruit Ninja subsequently rendered 2,102
frames without a guest fault in the comparison run. TLSF continues to reclaim
storage, and clearing writes go through Unicorn to keep executable memory
coherent.

The final follow-up build also passed the first frame, rendered the menu, and
entered Zen gameplay. Its captures are saved directly in the emulator photo
gallery.

Android's SDL Java layer already forwards individual pointer IDs. The native
host discarded additional presses, shared one held flag across all fingers,
and stubbed `TouchEventInfo::GetPointId` to zero. It now tracks SDL device and
finger IDs separately, assigns stable guest IDs for each contact, and delivers
each press, move and release through the existing Bada listener. The control's
`SetMultipointEnabled` setting is retained. Synthetic mouse/finger copies are
ignored, and background/focus loss releases every held contact at its last
position.

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
- touchHLE's [SDL multi-touch handling](https://github.com/touchHLE/touchHLE/blob/ca1de89c578e79746f33c1332e28efd106c334ba/src/window.rs)
  and zeroed allocation payloads in the `mem.rs` source above.
- Cocos2d-x's [Bada touch implementation](https://github.com/cocos2d-x/cocos2dx-webos/blob/293ca322978533729a7ef99d49d7705cf90ae34b/cocos2dx/platform/bada/CCEGLView_bada.cpp),
  which enables multi-point input and uses `GetPointId` for per-contact
  press/release dispatch. These sources were found with `gh search code`.
- [PowerVR decoder](https://github.com/powervr-graphics/Native_SDK/tree/2b1bf2f14d3365d0bb801e2a6a131a319d3a2e48/framework/PVRCore/texture), commit
  `2b1bf2f14d3365d0bb801e2a6a131a319d3a2e48` (MIT, unmodified; the decoder
  matches the previously vendored `63f0098` snapshot).
- [TLSF pool allocator](https://github.com/mattconte/tlsf/tree/deff9ab509341f264addbd3c8ada533678591905), commit
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
The follow-up checks passed for heap recycling and cleared payloads, mapped
memory copies and overlap, watchdogs, UTF-16, files/checkpoints, audio/timers,
and pixel expansion including page-protected guest buffers. The multi-touch
probe queues real SDL finger events and verifies Bada IDs, separate devices,
mouse isolation, cancellation, and primary-finger behavior in single-point
apps. ARM/Thumb ABI checks passed in the initial investigation.

An unprofiled 800×480 gameplay run before the final alignment correction
reached the first-person tutorial. Joystick
input changed the player's position, firing produced visible shots, and the
rifle magazine decreased to 13 rounds. The runner observed a peak game process
RSS of about 167 MiB. Gameplay measured approximately **7–8.5 FPS**, with
roughly **114–140 ms of busy time per frame**, using llvmpipe and
`LP_NUM_THREADS=8`. These measurements describe this Linux/proot
software-rendering host. The maintainer subsequently tested the initial PR
build on their device and reported stable **30 FPS** in Nova; that device
result is independent of the headless measurements here.

Use `BADA_TRACE_FPS=1` to measure actual frame throughput and busy time. Nova
re-arms a 5 ms timer; that interval is not a guarantee of rendered FPS. The ARM
guest executes on one thread. Native Android uses its GLES driver; headless
desktop tests use llvmpipe, whose rendering worker count can be set separately
with `LP_NUM_THREADS`. Mesa's [environment-variable documentation](https://github.com/marchaesen/vcxsrv/blob/d0a1eaf7ee15fcdf4f683388a88fec49078e6408/mesalib/docs/envvars.rst)
describes that option. Keep compilation and game runs separate when testing on
memory-constrained hosts.

Emulator captures for this investigation are saved directly in
`/root/emulator photo gallery`.
