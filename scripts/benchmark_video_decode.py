#!/usr/bin/env python3
"""Compare FFmpeg software/NVDEC decoding, with optional CPU RGB conversion.

No window, audio, deinterlacing, GTK rendering or DVB demodulation is included.
CUDA failures are reported as failures, never as software fallback results.
"""
import argparse
import json
from pathlib import Path
import resource
import shlex
import statistics
import subprocess
import time
from fractions import Fraction


MODES = {
    "cpu": ([], []),
    "cuda": (["-hwaccel", "cuda", "-hwaccel_output_format", "cuda"], []),
    "cpu_rgb": ([], ["-vf", "format=bgra"]),
    "cuda_rgb": (["-hwaccel", "cuda", "-hwaccel_output_format", "cuda"],
                 ["-vf", "hwdownload,format=nv12,format=bgra"]),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--realtime", action="store_true")
    parser.add_argument("--fps", type=Fraction,
                        help="Known picture rate, e.g. 30000/1001 (raw streams may report 25)")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    # Preserve earlier measurements; use a new output directory for each run.
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    failed = False
    for input_index, path in enumerate(args.inputs):
        path = path.resolve()
        probe = subprocess.run([
            "ffprobe", "-v", "fatal", "-select_streams", "v:0",
            "-show_entries", "stream=codec_name,width,height,pix_fmt,avg_frame_rate",
            "-of", "json", str(path),
        ], check=True, capture_output=True, text=True)
        stream = json.loads(probe.stdout)["streams"][0]
        if stream["codec_name"] not in ("h264", "hevc"):
            parser.error("This comparison supports H.264 and HEVC inputs")
        if stream["pix_fmt"] != "yuv420p":
            parser.error("RGB comparison currently requires 8-bit yuv420p input")
        fps = float(args.fps if args.fps is not None else Fraction(stream["avg_frame_rate"]))
        if fps <= 0:
            parser.error("Input must have a known positive frame rate")
        for repeat in range(args.repeats):
            # Alternate order to reduce systematic warm-up/thermal bias.
            modes = list(MODES.items())
            if repeat % 2:
                modes.reverse()
            for mode, (decoder, filters) in modes:
                command = [
                    "ffmpeg", "-hide_banner", "-nostdin", "-nostats",
                    "-loglevel", "warning", "-threads", "1", *decoder,
                    *(["-readrate", "1"] if args.realtime else []),
                    "-i", str(path), "-map", "0:v:0", "-an", "-sn", "-dn",
                    "-filter_threads", "1", *filters, "-fps_mode", "passthrough",
                    "-progress", "pipe:1", "-f", "null", "-",
                ]
                stem = f"{input_index}-{path.stem}-{mode}-{repeat + 1}"
                before = resource.getrusage(resource.RUSAGE_CHILDREN)
                start = time.monotonic()
                with (args.output / f"{stem}.log").open("w") as log:
                    log.write(shlex.join(command) + "\n")
                    log.flush()
                    run = subprocess.run(command, stdout=subprocess.PIPE,
                                         stderr=log, text=True, timeout=300)
                wall = time.monotonic() - start
                after = resource.getrusage(resource.RUSAGE_CHILDREN)
                cpu = (after.ru_utime + after.ru_stime -
                       before.ru_utime - before.ru_stime)
                progress = dict(line.split("=", 1) for line in run.stdout.splitlines()
                                if "=" in line)
                frames = int(progress.get("frame", "0"))
                ok = run.returncode == 0 and frames > 0 and progress.get("progress") == "end"
                diagnostics = (args.output / f"{stem}.log").read_text().splitlines()[1:]
                failed |= not ok
                seconds = frames / fps
                row = dict(input=str(path), stream=stream, fps_used=fps, mode=mode,
                           repeat=repeat + 1, command=command, exit_code=run.returncode,
                           ok=ok, diagnostic_lines=len(diagnostics),
                           frames=frames, video_seconds=seconds,
                           cpu_seconds=cpu, wall_seconds=wall,
                           core_percent=100 * cpu / seconds if ok else None)
                results.append(row)
                (args.output / "results.json").write_text(json.dumps({
                    "realtime": args.realtime, "results": results,
                }, indent=2) + "\n")
                print(f"{path.name} {mode} #{repeat + 1}: frames={frames} "
                      f"CPU={cpu:.3f}s wall={wall:.3f}s ok={ok} "
                      f"diagnostic_lines={len(diagnostics)}", flush=True)
        counts = {r["frames"] for r in results if r["input"] == str(path) and r["ok"]}
        if len(counts) != 1:
            failed = True
            print(f"WARNING: unequal frame counts for {path.name}: {counts}", flush=True)
        for mode in MODES:
            rows = [r for r in results if r["input"] == str(path) and r["mode"] == mode]
            if all(r["ok"] for r in rows):
                median = statistics.median(r["core_percent"] for r in rows)
                print(f"{path.name} {mode}: median CPU/video-time = {median:.1f}% "
                      "of one CPU core", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
