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

## Validation

The title, mode menu and Quick Game render correctly. Touch input enters the
game and responds to repeated drops, including a tower counter of two.
The final unprofiled run exited normally after 108.2 seconds and peaked at
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
