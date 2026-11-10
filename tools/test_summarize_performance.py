import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("summary", Path(__file__).with_name("summarize-performance.py"))
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


class PerformanceSummaryTests(unittest.TestCase):
    def test_reset_weighting_and_raw_percentiles(self):
        # Unequal window lengths require total frames / total time, not the
        # arithmetic mean of FPS. A reset must discard the loading frame.
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "game.log"
            log.write_text("[perf] t=1 window_ms=1000 frames=1 fps=1 texture_bytes=0 vertex_bytes=0 underrun_frames=0 gc_ms=na\n"
                           "[perf] enabled: reset\n"
                           "[perf] t=1 window_ms=1000 frames=60 fps=60 cpu_ms=2 texture_bytes=0 vertex_bytes=128 underrun_frames=0 gc_ms=na\n"
                           "[perf] t=3 window_ms=2000 frames=30 fps=15 cpu_ms=8 texture_bytes=0 vertex_bytes=256 underrun_frames=5 gc_ms=na\n")
            log.with_suffix(".csv").write_text("frame_ms\n10\n20\n30\n40\n")
            result = summary.summarize(log)
            self.assertEqual(result["fps"], 30)
            self.assertEqual(result["time_per_frame"]["cpu_ms"], 4)
            self.assertEqual(result["frame_ms"]["p50"], 20)
            self.assertEqual(result["frame_ms"]["p95"], 40)
            self.assertEqual(result["underrun_frames"], 5)

    def test_missing_samples_do_not_become_zero_stutter(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "game.log"
            log.write_text("[perf] enabled: reset\n")
            with self.assertRaises(ValueError):
                summary.summarize(log)


if __name__ == "__main__":
    unittest.main()
