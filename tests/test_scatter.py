#!/usr/bin/env python3
"""Exercise the embedded recovery/relay using real H.264 and H.265 TS."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ScatterTests(unittest.TestCase):
    def test_recovery_and_unchanged_player_stream(self):
        for codec in ("libx264", "libx265"):
            with self.subTest(codec=codec), tempfile.TemporaryDirectory() as tmp:
                source = pathlib.Path(tmp, "source.ts")
                output = pathlib.Path(tmp, "player.ts")
                args = ["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                        "testsrc2=size=320x240:rate=2", "-t", "2", "-an",
                        "-c:v", codec, "-threads", "1", "-preset", "ultrafast",
                        "-g", "1"]
                if codec == "libx265":
                    args += ["-x265-params", "pools=none:frame-threads=1:log-level=error"]
                args += ["-metadata", "service_provider=PE1ITR", "-metadata",
                         "service_name=Scatter test", "-f", "mpegts", str(source)]
                subprocess.run(args, check=True, capture_output=True, timeout=30)
                subprocess.run([ROOT / "build/scatter_probe", source, output],
                               check=True, capture_output=True, timeout=25)


if __name__ == "__main__":
    unittest.main()
