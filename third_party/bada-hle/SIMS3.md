# The Sims 3 compatibility

Investigated with the unmodified Bada 1.0.4 package (`AppId 68kei58gum`), whose
Marmalade wrapper is `Bin/s3e.exe` and whose game is `Data/The Sims 3.s3e`.
The package and ARM runtime are local inputs and are not included in the repo.

## Changes

- Deliver the startup foreground callback and run cooperative `IRunnable`
  workers with independent ARM/VFP contexts and stacks. Sleep, Yield, worker
  Join and semaphore waits return control to the host so queued UI work can
  finish. Worker destruction releases the context and stack. The scheduler
  remains cooperative; it does not implement arbitrary preemptive threading or
  blocking an unfinished worker from a host-driven main-thread callback.
- Implement Application user-event posting, the Osp realloc alias, memory/model
  queries, buffer capacity, String indexing and directory enumeration. `/Home`
  enumeration merges writable files with package Data, allowing the wrapper to
  discover the game executable; matching overlay names take precedence.
- Populate Canvas lock descriptors. Explicit Frame orientation changes update
  the surface and notify Form listeners on the host event loop. This gives the
  wrapper an 800x480 landscape surface and matching touch coordinates.
- Expose EGL configuration discovery and accurate attributes for one window
  configuration supporting GLES 1 and 2. The old fallback selected shader calls
  without creating a usable shader program. Android honors the client version
  requested at `sglCreateContext`, since the wrapper imports both GL APIs.
- Implement the exercised GLES 1 fixed-point state and matrix operations.
  Desktop client/VBO `GL_FIXED` arrays are converted to floats while queries
  continue to report the guest descriptor. Native ES uses its fixed arrays.
- Return the actual contact position from TouchEventInfo. The runtime reads
  this accessor instead of the Point argument supplied to its listener.
- Implement native Keypad sessions, text limits, acceptance/cancellation and
  virtual destruction. Desktop text input stays responsive while the guest
  drawing worker waits; Android uses an Activity dialog and queues results back
  onto the emulator thread. Session identities reject stale input after a
  control is replaced. EditField retains its per-keystroke callbacks.
- Implement String emptiness and `StringUtil::StringToUtf8N`, including its
  NUL-terminated ByteBuffer and owned-result cleanup. Marmalade copies this
  buffer after name entry; a fallback object previously produced an invalid name.
- Return `E_SUCCESS` from successful raw `File::Write` calls, as specified by
  Bada. Marmalade converts that result into a byte count; returning the count
  directly made the wrapper report a failed write. File tests verify the result
  code and the bytes read back after closing the file.

## Validation

Native regression tests require SDL2, GLES 1/2, desktop GL and Unicorn:

```sh
cmake -S frontends/pocket-android/tests/native -B build/native-tests -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBADA_BUILD_UNICORN=OFF
cmake --build build/native-tests -j 2
LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 SDL_AUDIODRIVER=dummy \
  xvfb-run -a ctest --test-dir build/native-tests --output-on-failure
```

The suite exercises worker suspension and joins, core/VFP isolation, semaphore
token transfer and timeout, directory discovery and cleanup, buffer capacity,
touch positions, EGL client selection, fixed-point client/VBO drawing and
orientation callbacks/Canvas descriptors. Keypad tests cover delayed completion,
cancellation, limits, virtual cleanup, UTF-16 to UTF-8 conversion and existing
EditField callbacks. On Linux aarch64 with Unicorn 2.1.1,
14 tests pass; the two native ES 1 cases skip because Mesa cannot create an ES 1
context. Desktop compatibility GL covers the fixed-function draws in both ABIs.

The desktop engine was built with the pinned Unicorn source. The game reaches
the landscape menu, completes Create-a-Sim (appearance, persona, five traits
and the name `Alex`), and enters the 3D home scene with the gameplay HUD.
The game clock advances and in-game dialogs/menu input respond. Persistent
save/load remains unsupported in this validation: saving displays an `Alex`
slot, but selecting it returns to Create-a-Sim and no save file appears in the
writable Home directory. This change establishes startup and initial gameplay
compatibility, rather than full-game compatibility.
Fruit Ninja, Sky Force and N.O.V.A. smoke runs
render their menu/language/title screens without guest faults or HLE exceptions.
Fruit Ninja's presentation matches the existing baseline build. Android host
code passes a syntax check and all Kotlin sources compile against cached
Android/AndroidX APIs; gameplay on an Android device remains unverified.

Under this Xvfb/Mesa configuration, front-buffer readback produces black
captures. Local validation captured the back buffer instead; this workaround
is outside the production code.

## References

API documentation and implementations were located with `gh search code`:

- [Bada 1.0 API reference](https://github.com/romanm11/bada-SDK-1.0.0-Help),
  including Thread, Semaphore, DirEnumerator, DirEntry and BufferInfo.
- [Cocos2d-x Bada pthread implementation](https://github.com/cocos2d-x/cocos2dx-webos/blob/293ca322978533729a7ef99d49d7705cf90ae34b/cocos2dx/platform/bada/pthread.cpp).
- [Tizen successor orientation definitions](https://github.com/kitsilanosoftware/CppSharpTizen/blob/92e42a36cc3c5f2b1636061e82025feec4edda0d/tizen-sdk/platforms/tizen2.1/rootstraps/tizen-emulator-2.1.native/usr/include/osp/FUiIOrientationEventListener.h),
  checked against the Bada 1.0 reference.
- [Marmalade loader format notes](https://github.com/knot126/Marmalade-Modding/blob/ddad72228b8f30d9e1e80e3dd16c2cd3b9775296/docs/misc/IwS3ERead.h).
