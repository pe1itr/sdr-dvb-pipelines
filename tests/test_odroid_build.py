"""ODROID setup planning: no apt installation, radio or network access."""
import configparser
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("odroid_build", ROOT / "tools/build_odroid_n2.py")
helper = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(helper)


class OdroidTests(unittest.TestCase):
    def invoke(self, args=(), *, runtime_error=None, command_error=None):
        with patch.object(helper, "require_platform"), patch.object(helper, "run", side_effect=command_error) as run, \
                patch.object(helper, "check_repeater", side_effect=runtime_error) as check, \
                patch.object(helper, "install_dependencies") as install, patch.object(helper, "write_config") as config, \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = helper.main(list(args))
        return result, run, check, install, config

    def test_check_is_read_only_and_uses_isolated_directory(self):
        result, run, _, install, config = self.invoke(["--check"])
        self.assertEqual(result, 0)
        install.assert_not_called()
        config.assert_not_called()
        run.assert_called_once()
        self.assertIn("--check", run.call_args.args[0])
        self.assertIn("build-odroid-n2", run.call_args.args[0])

    def test_build_defaults_and_explicit_package_install(self):
        for flags, expected in (([], 0), (["--install-deps", "-j", "2"], 1)):
            with self.subTest(flags=flags):
                result, run, check, install, config = self.invoke(flags)
                self.assertEqual(result, 0)
                self.assertEqual(install.call_count, expected)
                self.assertEqual(run.call_count, 2)
                check.assert_called_once_with("ffmpeg")
                config.assert_called_once()
                self.assertNotIn("--check", run.call_args.args[0])
                self.assertIn("-march=armv8-a", run.call_args.kwargs["env"]["CFLAGS"])
                self.assertNotIn("-march=native", run.call_args.kwargs["env"]["CFLAGS"])

    def test_failed_dependency_or_runtime_blocks_build_and_config(self):
        for kwargs in ({"runtime_error": ValueError("SRT missing")},
                       {"command_error": subprocess.CalledProcessError(1, "check")}):
            result, run, _, _, config = self.invoke(**kwargs)
            self.assertEqual(result, 1)
            self.assertEqual(run.call_count, 1)
            config.assert_not_called()

    def test_wrong_architecture_is_rejected(self):
        for machine in ("x86_64", "armv7l"):
            with patch.object(helper.platform, "machine", return_value=machine):
                with self.assertRaisesRegex(ValueError, "aarch64"):
                    helper.require_platform()

    def test_focal_release_and_compiler_flags(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "os-release")
            path.write_text('NAME="Ubuntu"\nVERSION_ID="20.04"\nID=ubuntu\nID_LIKE=debian\n')
            self.assertEqual(helper.os_release(path)["VERSION_ID"], "20.04")
        with patch.dict(os.environ, {"CFLAGS": "-O1 -g", "CC": "ccache gcc"}, clear=True):
            env = helper.build_environment()
            self.assertTrue(env["CFLAGS"].startswith("-O1 -g "))
            self.assertIn("-mtune=cortex-a73.cortex-a53", env["CXXFLAGS"])
            self.assertEqual(env["CC"], "ccache gcc")

    def test_config_preserves_existing_and_corrects_helper_paths(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            path = Path(tmp, "pi6ehv.ini")
            helper.write_config(path, "/usr/bin/ffmpeg")
            cfg = configparser.ConfigParser(interpolation=None)
            cfg.read(path)
            self.assertEqual(cfg["session"]["source"], "rtlsdr")
            self.assertEqual(cfg["session"]["renderer"], "srt")
            self.assertEqual(cfg["rtlsdr"]["frequency"], "0")
            self.assertEqual(cfg["srt"]["x264_preset"], "ultrafast")
            self.assertEqual(cfg["srt"]["video_threads"], "3")
            self.assertEqual(cfg["srt"]["filter_threads"], "1")
            self.assertEqual(cfg["paths"]["sdr"], str(ROOT / "build-odroid-n2/sdr_to_stdout"))
            path.write_text("user settings\n")
            helper.write_config(path, "/usr/bin/ffmpeg")
            self.assertEqual(path.read_text(), "user settings\n")

    def test_runtime_requires_output_protocol_and_driver(self):
        for protocols, factories, ok in (("Input:\n srt\nOutput:\n udp", "rtlsdr", False),
                                         ("Input:\nOutput:\n srt", "hackrf", False),
                                         ("Input:\nOutput:\n srt", "rtlsdr", True)):
            results = [subprocess.CompletedProcess([], 0, protocols),
                       subprocess.CompletedProcess([], 0, " V..... libx264 H264\n A..... aac AAC\n"),
                       subprocess.CompletedProcess([], 0, "Available factories... " + factories)]
            with patch.object(helper, "run", side_effect=results), contextlib.redirect_stdout(io.StringIO()):
                if ok:
                    helper.check_repeater("ffmpeg")
                else:
                    with self.assertRaises(ValueError):
                        helper.check_repeater("ffmpeg")

    def test_local_ffmpeg_is_pinned_and_installed_only_in_build_directory(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(helper, "ROOT", Path(tmp)), \
                patch.object(helper, "run", return_value=subprocess.CompletedProcess([], 0, helper.FFMPEG_COMMIT + "\n")) as run:
            binary = helper.local_ffmpeg(1)
            self.assertEqual(binary, str(Path(tmp) / helper.BUILD_DIR / "ffmpeg/bin/ffmpeg"))
            commands = [call.args[0] for call in run.call_args_list]
            self.assertIn(helper.FFMPEG_TAG, commands[0])
            self.assertIn("--enable-libsrt", commands[2])
            self.assertIn("--enable-libx264", commands[2])
            self.assertIn("--prefix=" + str(Path(tmp) / helper.BUILD_DIR / "ffmpeg"), commands[2])
            self.assertEqual(commands[-1], ["make", "install"])
        with tempfile.TemporaryDirectory() as tmp, patch.object(helper, "ROOT", Path(tmp)), \
                patch.object(helper, "run", return_value=subprocess.CompletedProcess([], 0, "wrong-commit\n")) as run:
            with self.assertRaisesRegex(ValueError, "Unexpected FFmpeg"):
                helper.local_ffmpeg(1)
            self.assertEqual(run.call_count, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
