#!/usr/bin/env python3
"""Summarize --profile logs and optional BADA_PROFILE_FRAMES CSV samples."""
import argparse
import csv
import json
import math
from pathlib import Path


def percentile(values, fraction):
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)] if values else None


def summarize(log_path, frame_path=None):
    rows = []
    faults = []
    for line in Path(log_path).read_text(errors="replace").splitlines():
        if line.startswith("[perf] enabled:"):
            rows.clear()  # profile reset deliberately discards loading/warmup
        elif line.startswith("[perf] t="):
            row = {}
            for item in line.split()[1:]:
                key, value = item.split("=", 1)
                try:
                    row[key] = float(value)
                except ValueError:
                    row[key] = value
            rows.append(row)
        elif "[cpu] emu stopped" in line or "[hle] EXCEPTION" in line:
            faults.append(line)
    if not rows:
        raise ValueError(f"No per-second profile samples in {log_path}")
    seconds = sum(row["window_ms"] for row in rows) / 1000
    frames = sum(row["frames"] for row in rows)
    times = {}
    for key in ("cpu_ms", "hle_ms", "render_ms", "draw_ms", "state_ms", "upload_ms", "shader_ms",
                "present_ms", "query_ms", "canvas_ms", "audio_ms", "audio_mix_ms", "soft_float_ms"):
        times[key] = sum(row.get(key, 0) * max(row["frames"], 1) for row in rows) / max(frames, 1)
    frame_path = Path(frame_path) if frame_path else Path(log_path).with_suffix(".csv")
    durations = []
    if frame_path.exists():
        with frame_path.open(newline="") as stream:
            durations = [float(row["frame_ms"]) for row in csv.DictReader(stream)]
    return {
        "log": str(log_path), "seconds": seconds, "frames": int(frames), "fps": frames / seconds,
        "min_window_fps": min(row["fps"] for row in rows),
        "frame_ms": {
            "samples": len(durations), "mean": sum(durations) / len(durations) if durations else None,
            "p50": percentile(durations, .50), "p95": percentile(durations, .95),
            "p99": percentile(durations, .99), "max": max(durations) if durations else None,
        },
        "time_per_frame": times,
        "calls_per_frame": {key: sum(row.get(key, 0) for row in rows) / max(frames, 1)
                            for key in ("draw_calls", "state_calls", "upload_calls", "shader_calls", "allocs")},
        "texture_bytes_per_second": sum(row["texture_bytes"] for row in rows) / seconds,
        "vertex_bytes_per_second": sum(row["vertex_bytes"] for row in rows) / seconds,
        "underrun_frames": int(sum(row["underrun_frames"] for row in rows)),
        "gc": "ART counters" if any(isinstance(row["gc_ms"], float) for row in rows) else rows[0]["gc_ms"],
        "faults": faults,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--require-fps", type=float, help="Fail unless every reported window meets this FPS")
    args = parser.parse_args()
    summaries = [summarize(path) for path in args.logs]
    print(json.dumps(summaries, indent=2))
    if any(result["faults"] for result in summaries):
        raise SystemExit(1)
    if args.require_fps is not None and any(result["min_window_fps"] < args.require_fps for result in summaries):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
