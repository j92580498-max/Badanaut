# Game performance investigation

This change is a work in progress. The target is stable 60 FPS with no audio
underruns on Helio G99 / Mali, using Fruit Ninja as the regression reference.
That target has not been verified. There is no connected Android device in the
current test environment.

## Measurements recovered before optimization

The available measurements used a Linux ARM64 host under PRoot, Xvfb,
single-threaded Mesa software rendering (`LIBGL_ALWAYS_SOFTWARE=1`,
`LP_NUM_THREADS=1`), dummy SDL audio and disabled vsync. They are useful for
finding work performed by the emulator; they do not measure the Mali driver or
establish the user's Android 60 FPS baseline. Times below are wall-clock CPU
times, including driver stalls, rather than GPU timer-query measurements.

| Scene | FPS | Mean frame | p95 frame | Guest CPU / frame | Draws / frame | State calls / frame | `glFinish` / frame |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Fruit Ninja animated menu, 40.03 s | 13.14 | 76.11 ms | 87.82 ms | 10.27 ms | 25.44 | 444.50 | 58.47 ms |
| Sims 3 loaded home with tutorial, 44.56 s | 4.60 | 217.31 ms | 224.76 ms | 108.51 ms | 251 | 3,812 | 84.73 ms |

Both warmed scenes recorded zero guest texture uploads and shader-compilation
calls. Sims recorded 1,517,819 missing audio sample frames in its measurement
window, compared with zero for Fruit Ninja. These are emulator queue underrun
counters with a dummy backend, not a recording of audible crackle.

Sims executes substantially more guest arithmetic, draws and GL state changes.
A separate diagnostic basic-block run found frequently executed ARM software
floating-point helpers and a four-term dot-product routine in a runtime module
loaded into guest heap memory. This is distinct from imported `__aeabi_*` calls;
an import-only profile reports zero soft-float time for that workload. Block
hooks perturb execution and their FPS must not be used for comparisons. The
guest already runs through Unicorn's translating JIT, rather than an
instruction interpreter.

## How the draft works

- `performance.cpp` records opt-in per-second presentation FPS, raw frame
  intervals, guest CPU/HLE time, GL categories, audio mixing, upload bytes,
  allocations and underruns. Android also samples ART GC work and blocking-GC
  counters. GL driver time is recorded separately when the render thread runs;
  concurrent timings must not be added together as a frame budget.
- Import binding resolves a stable dispatch entry once. Normal calls use the
  cached handler; string lookup, diagnostic counts and call logging are kept
  out of the normal dispatch path.
- The optional render worker owns the GL context and executes a bounded queue.
  Upload data is copied into reusable staging buffers before guest code can
  change it. Queries synchronize with the worker. `glFinish` still waits for GL
  completion, while the emulation thread services guest audio callbacks.
- Audio uses a preallocated PCM ring with per-buffer completion markers. SDL's
  playback callback mixes on its audio thread without per-buffer allocation.
  Long guest callbacks can yield to refill audio; Android prefers SDL's AAudio
  backend with OpenSL ES / Android fallback.
- Guest buffer/client-texture bindings are retained in host state to avoid
  querying the driver around each client-array draw. Vertex staging is reused.
- The experimental math path recognizes a complete dot-product routine and its
  arithmetic helpers by fingerprints. It checks code snapshots and supported
  finite inputs before executing native float arithmetic, and falls back to
  guest execution for changed code or unsupported values.
- `--internal-resolution=75` or `50` renders the default game surface into a
  smaller framebuffer and scales it for presentation. Guest screen dimensions
  remain unchanged. This path still needs rendering and state-restoration
  regression coverage. It currently has a command-line interface.

The render thread is **off by default**. An exploratory Sims run with it enabled
averaged 5.11 FPS / 196.60 ms, with p95 251.75 ms and 414,271 missing audio sample
frames over 29.96 seconds. A subsequent Fruit Ninja trial averaged 9.18 FPS /
108.88 ms, with p95 114.22 ms and 29,584 missing audio sample frames over 30.05
seconds. Scene timing and host conditions were not fully controlled between
trials. These results do not establish an improvement; the Fruit Ninja result
requires investigation before enabling the worker by default.

## Reproduce profiling

Build an optimized native engine, then run each game from the same scene with
the same presentation settings and device conditions:

```sh
BADA_PROFILE_FRAMES=/path/to/fruit.csv bada-hle /path/to/fruit --profile
BADA_PROFILE_FRAMES=/path/to/sims.csv bada-hle /path/to/sims --profile
python3 tools/summarize-performance.py /path/to/fruit.log /path/to/sims.log
```

Capture engine stdout into the corresponding `.log`. Use the existing control
interface's `profile reset` after loading and warmup, then `profile report` for
per-import totals. `profile stop` closes the raw frame CSV. A matching `.csv`
beside each log is used by the summary tool. `blocks start` / `blocks report`
are separate, invasive CPU diagnostics. To compare the experimental worker,
use `--render-thread=true`; `BADA_NO_MATH_ACCEL=1` disables math acceleration.

## Work required before this can be considered a 60 FPS fix

- Validate native ES 1 / ES 2 rendering, copied-command lifetimes, context
  replacement, lower-resolution presentation and saved guest GL state.
- Investigate the Fruit Ninja render-thread result and verify the default path
  against the unmodified baseline.
- Validate audio refill scheduling under long ARM and GL callbacks, including
  nested guest calls and guest workers, with no underruns.
- Add Android launcher controls for profiling and internal resolution, and
  complete optimized Android builds for both supported ABIs.
- Capture controlled, matched before/after gameplay profiles on Helio G99 /
  Mali, including cold loading and warmed gameplay. Confirm Fruit Ninja still
  sustains 60 FPS, inspect p95/p99 frame times and listen for audio crackle.

The local optimized host build succeeds. PCM ring and runtime-audio/file probes
have passed in earlier runs; the native-log probe encountered an unavailable
hard-coded `/tmp` directory in this PRoot session. Full graphics, Android-build
and hardware performance validation remain outstanding.
