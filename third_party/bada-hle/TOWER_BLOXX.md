# 3D Tower Bloxx Deluxe compatibility

Investigated with the unmodified ARM32 `TowerBloxx.exe`, AppId `367h34i51g`.
The game package and Bada C++ runtime remain local test inputs.

## Startup failures

- `File::GetName()` was missing. The resource loader obtains its filename
  through this method before querying file attributes. Returning zero left
  resource lengths invalid and eventually caused a huge host `memset`
  allocation. The implementation retains the opened guest path and constructs
  the returned Bada String through the ARM hidden result pointer.
- `SettingInfo::GetValue(String const&, bool&)` wrote four bytes into a
  one-byte output. `AppBridge::IsSilent` puts that boolean at `sp+23`, directly
  before saved r4 at `sp+24`. The word store changed the Game pointer to
  `0x80000000`, causing `Game::loadSettings()` to fault at `0x95a6a`. A byte
  write preserves the saved register.
- Bulk memory operations now reject unmapped or overflowing guest ranges and
  stage at most 64 KiB at a time. Overlapping moves retain their copy direction.
  Handler exceptions propagate as failed guest callbacks, and failed startup
  or frame callbacks stop the emulator with a failure exit status.
- Unicorn's translation cache is set to 64 MiB before JIT initialization,
  replacing its default 1 GiB reservation on 64-bit hosts.

The Digital Chocolate resource-offset layout was cross-checked using
`gh search code` against [Toolkit.java](https://github.com/dominguitox/Zombie-Lane-Revival-Client/blob/28d4cf9920c833dadb7f64f3dd082b92f5409201/src/game-ios/com/dchoc/toolkit/Toolkit.java).
No external game source or game binary changes are included.

## Frame-time fixes

Tower Bloxx repeatedly queries uniform and attribute names while drawing.
An opt-in `BADA_PROFILE_HLE=1` run measured about 18 seconds in those lookups
and their unconditional logging. Locations are now cached by program and
string contents. Relinking, deletion and context changes invalidate the
cache; unlinked or unknown programs still reach GL to preserve lookup errors.
`BADA_GLDBG=1` can log cache misses without flooding ordinary runs.

Constant shader names live in executable pages rather than the shared heap.
Guest string reads now use bounded chunks inside each mapped page, replacing
one Unicorn read per character while preserving requested lengths and page-end
behavior.

Before/after runs used the same single-core Linux/proot host, llvmpipe with
`LP_NUM_THREADS=1`, low process priority and bounded memory. Late gameplay
samples showed:

| Measurement | Before | After |
| --- | ---: | ---: |
| Median frame rate | 3.1 FPS | 4.6 FPS |
| Median busy frame time | 326.5 ms | 219.3 ms |
| Mean uniform lookup host time | 119.4 µs | 2.8 µs |
| Mean attribute lookup host time | 95.7 µs | 2.3 µs |

Each gameplay run exited normally. Shader lookup overhead fell by more than
95%; the remaining headless rendering time is dominated by software rendering
and presentation. These figures do not measure Android hardware GLES speed.
Fruit Ninja measured about 6.8 FPS in the same restricted software-rendering
comparison.

## Music playback

Tower Bloxx uses `Osp::Media::Player` for its MP3 music. The old Player only
simulated a 300 ms video clip: `OpenFile` and `OpenBuffer` ignored their input,
volume/mute/seek were success stubs, and asynchronous opens never notified the
registered listener. No compressed audio reached SDL.

Player now decodes MP3 and WAV with the pinned dr_libs memory decoders and mixes
them through the same SDL playback device as `AudioOut`. The audio callback
reads host-owned encoded bytes into fixed 1024-frame PCM blocks, resamples to
the device rate and advances playback independently of the guest frame loop.
It supports pause/resume, looping, volume, mute and seeking. `OpenFile` uses
the package's case-insensitive path lookup; `OpenBuffer` copies the bytes before
the game can reuse or destroy them.

Encoded audio is limited to 16 MiB per clip and 64 MiB across at most 64 open
clips, with a maximum one-hour duration. There is no full-song PCM allocation.
Unsupported AVI/MP4 intros retain their short completion placeholder; only
their container header is read, including for movies larger than the audio
limit. Closing or destroying a Player releases its input and invalidates queued
callbacks. Open, seek and completion listeners run on the emulation thread,
preserving the listener subobject and its Thumb adjustment thunk.

Native disk-audio probes verify the actual MP3/WAV samples, stereo channel
order, resampling, source ownership, seeking after EOF, suspension, pause,
looping, volume/mute, clipping while mixing with `AudioOut`, and callback
invalidation after close/destruction. The synthesized 100 ms MP3 fixture is
original test data, including LAME delay/padding metadata; an independent
mpg123 decode confirmed its 4410 stereo frames at 44.1 kHz.

The game opens `/Res/19.mp3` for menu music and `/Res/22.mp3` in Quick Game,
with WAV effects opened from ByteBuffers. The end-to-end run recorded 80.448
seconds of 48 kHz stereo S16 output, including 74.981 seconds with samples above
32 in either channel. Every one-second window contained audio; the short WAV
effects alone cannot account for this continuous output. The game accepted
repeated drops and reached a tower counter of two. It exited normally after
103.0 seconds with no recorded guest faults, peaking at 247.7 MiB for the
emulator, Xvfb and capture tools together. Screenshots are in
`/root/emulator photo gallery`, and the run used copied saves in an isolated
test package.

Late-game median performance remained 4.6 FPS and 219.3 ms of busy frame time
on this restricted software-rendering host. Fruit Ninja completed a further
180-frame regression run normally in 49.6 seconds with no recorded guest
faults, peaking at 230.6 MiB.

## Validation

The title, mode menu and Quick Game render correctly. Touch input enters the
game and responds to repeated drops, including a tower counter of two.
The earlier startup/rendering verification exited normally after 108.2 seconds
and peaked at
269.8 MiB for the emulator, Xvfb and capture tools together. Captures are saved
in `/root/emulator photo gallery`.

Native regressions cover the filename/attribute lookup, one-byte output and
saved-register preservation, large overlapping transfers, callback failure
recovery and string reads across mapped and unmapped page boundaries. Rendering
probes also exercise mutable shader names, successful and failed relinks,
pending attribute bindings and deleted programs.

All 16 applicable native probes passed, including native GLES 2 and desktop
ES 1/2 handlers with both guest float ABIs. The two native ES 1 probes skipped
because this Mesa host cannot create that context, before guest code runs.
Fruit Ninja also rendered its menu for 180 frames and exited normally in the
final regression run, with no recorded guest faults.
