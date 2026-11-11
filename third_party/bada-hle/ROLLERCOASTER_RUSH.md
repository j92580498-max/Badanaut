# 3D Rollercoaster Rush Jurassic compatibility

Investigated with the unmodified ARM32 executable `Bin/CityGuide.exe`, AppId
`pug9r81wwu`. The supplied archive's metadata names a different app; the
rendered title identifies 3D Rollercoaster Rush Jurassic. The archive, firmware,
copied saves, recordings and screenshots remain local test inputs/outputs.

## Startup and timer dispatch

The original run drew one Digital Chocolate splash frame and stopped advancing.
The game constructs an event-driven `Osp::Base::Runtime::Thread`, whose
`OnStart()` arms a timer. Only the `IRunnable` overload of `Thread::Construct`
was implemented, so the event loop never started. The host also selected an
arbitrary `OnTimerExpired` symbol from the executable, choosing the application's
callback instead of the registered `CTimerThread` listener.

The `ThreadType` overload now supports worker and event-driven threads. Event
threads run `OnStart`, retain their event loop when it returns true, and deliver
timers on their own stack with the correct current-thread identity. `Stop`
queues `OnStop` and cancels pending timer events. Existing cooperative worker
sleep, join and semaphore behavior is preserved.

Timers retain their registered listener subobject, owner and one-shot deadline.
The host dispatches the listener's vtable, preserving multiple-inheritance and
Thumb adjustment thunks. A timer constructed before its event thread starts
binds to that thread when `OnStart` arms it; a foreground restart retains the
owner. Multiple timers are independent. Cancellation, restart and destruction
invalidate both queued and snapshotted events, including reused timer addresses.
Apps without a registered timer retain the existing single bootstrap callback.

The declarations were cross-checked with `gh search code` against the published
[legacy SDK Thread header](https://github.com/kitsilanosoftware/CppSharpTizen/blob/92e42a36cc3c5f2b1636061e82025feec4edda0d/tizen-sdk/platforms/tizen2.1/rootstraps/tizen-emulator-2.1.native/usr/include/osp/FBaseRtThread.h).
The game binary's relocations confirm the relevant virtual callback slots.

## Arithmetic and texture queries

The game uses `__aeabi_ldivmod` and `__aeabi_uldivmod`. Both helpers now return
the 64-bit quotient in r0:r1 and remainder in r2:r3, including for hard-float
guests. Unsigned magnitude arithmetic handles signed overflow and zero divisors
without host undefined behavior. The existing SystemTime millisecond units are
unchanged.

`glIsTexture_1` and `glIsTexture_2` now query the host GL texture identity instead
of returning an unconditional false result. Generation, binding and deletion
retain GL's texture-name semantics.

## Sound settings

The copied game initially showed both Music Off and Sound Effects Off in its
Settings page. Enabling them through the UI makes it open `/Res/161.wav` for
menu music and `/Res/162.wav` when entering a track. These are 22050 Hz mono
PCM WAV files lasting 40.987 and 56.530 seconds. They use the shared streamed
Player implementation described in [Tower Bloxx's music notes](TOWER_BLOXX.md).
No game-specific music hook or asset modification is required.

The completed session recorded 314.923 seconds of 48 kHz stereo S16 output,
including 309.055 seconds with samples above 32 in either channel. All 315
one-second windows contained audio. The short effects alone cannot account for
this continuous output.

## Live verification

The title, Options, Settings and Career map render correctly. The supplied
copy's selected track loads and shows its new-cart message. Touch dismisses the
message, accelerates the cart, applies the brake and restarts after an in-game
cart crash. Later captures show the passengers travelling along the track.
The run exited normally after 369.8 seconds with 1376 frames and no recorded
guest faults, peaking at 306.9 MiB for the emulator, Xvfb and capture helper
together.

The original baseline stopped after one splash frame, so there is no meaningful
baseline gameplay frame rate. Final late-game samples measured a median 5.2 FPS
and 189.5 ms of busy frame time. A loading callback took approximately 37 seconds
on this host. These measurements use software GL and do not establish Android
hardware GLES performance; loading and rendering remain slow on this setup.

Screenshots are saved in `/root/emulator photo gallery`. Live verification uses
an isolated package copy, one CPU at nice 10, software GL with one rendering
worker, and explicit memory, file-size and time limits.

Native probes cover worker/event lifecycle and context preservation, adjusted
timer listener pointers, one-shot delivery and deadlines, multiple timers,
cancellation, destruction and address reuse. Arithmetic probes exercise
ARM/Thumb callers and both float ABIs with wide signed/unsigned inputs, zero
divisors, signed overflow and register preservation. GL probes check generated,
bound, invalid and deleted texture names through the ES 1/2 handlers.

The full native rebuild completed under the same single-CPU limit. All 17
applicable probes passed, including Player disk-audio playback, event workers,
64-bit ABI helpers, native GLES 2 and desktop ES 1/2 with both guest float ABIs.
The two native ES 1 probes skipped because this Mesa host cannot create that
context, before guest code runs.
