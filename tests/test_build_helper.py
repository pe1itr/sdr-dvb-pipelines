"""Build preflight failures must block make; optional failures must not."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("build_helper", ROOT / "tools/build.py")
helper = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(helper)


class BuildHelperTests(unittest.TestCase):
    def run_helper(self, args=(), missing_library=None, missing_programs=(),
                   broken_header=None, make_status=0, environment=None):
        def command(argv):
            if argv[:3] == ["pkg-config", "--print-errors", "--exists"]:
                return (False, "package not found") if argv[-1] == missing_library else (True, "")
            if argv == ["SoapySDRUtil", "--info"]:
                return True, "Available factories... hackrf, rtlsdr"  # rfspace is optional
            return True, ""

        def compile_probe(directory, compiler, flags, source, suffix, link_flags):
            if broken_header and f"<{broken_header}>" in source:
                return False, "header or linker input missing"
            return True, ""

        output = io.StringIO()
        with patch.dict(os.environ, environment or {}, clear=True), \
                patch.object(helper.sys, "platform", "linux"), \
                patch.object(helper.shutil, "which",
                             side_effect=lambda name: None if name in missing_programs else f"/bin/{name}"), \
                patch.object(helper.Checker, "command", side_effect=command), \
                patch.object(helper.Checker, "compile", side_effect=compile_probe), \
                patch.object(helper.subprocess, "call", return_value=make_status) as make, \
                contextlib.redirect_stdout(output):
            status = helper.main(list(args))
        return status, output.getvalue(), make

    def test_missing_required_library_blocks_build(self):
        status, output, make = self.run_helper(missing_library="uhd")
        self.assertEqual(status, 1)
        self.assertIn("[ERROR] uhd", output)
        make.assert_not_called()

    def test_metadata_without_working_headers_or_linker_blocks_build(self):
        status, output, make = self.run_helper(broken_header="vlc/vlc.h")
        self.assertEqual(status, 1)
        self.assertIn("[ERROR] libvlc", output)
        make.assert_not_called()

    def test_missing_build_tool_blocks_build(self):
        for program in ("cc", "g++", "pkg-config", "make"):
            with self.subTest(program=program):
                status, output, make = self.run_helper(missing_programs=(program,))
                self.assertEqual(status, 1)
                self.assertIn("executable missing", output)
                make.assert_not_called()

    def test_optional_program_and_driver_only_warn(self):
        status, output, make = self.run_helper(missing_programs=("ffplay",))
        self.assertEqual(status, 0)
        self.assertIn("[WARNING] ffplay missing", output)
        self.assertIn("[WARNING] Soapy driver rfspace missing", output)
        make.assert_called_once()

    def test_missing_limesuite_is_optional(self):
        status, output, make = self.run_helper(missing_library="LimeSuite")
        self.assertEqual(status, 0)
        self.assertIn("LimeSDR Mini RX unavailable", output)
        make.assert_called_once()

    def test_check_never_runs_make_even_with_warnings(self):
        status, output, make = self.run_helper(("--check",), missing_programs=("SoapySDRUtil",))
        self.assertEqual(status, 0)
        self.assertIn("cannot check optional Soapy drivers", output)
        make.assert_not_called()

    def test_make_failure_and_checked_environment_are_preserved(self):
        status, output, make = self.run_helper(("-j", "3"), make_status=2,
                                             environment={"CC": "ccache clang", "CFLAGS": "-g -std=c11"})
        self.assertEqual(status, 2)
        argv = make.call_args.args[0]
        self.assertIn("-j3", argv)
        self.assertIn("CC=ccache clang", argv)
        self.assertIn("CFLAGS=-g -std=c11", argv)
        self.assertEqual(make.call_args.kwargs["cwd"], ROOT)

    def test_invalid_job_count_is_rejected(self):
        for value in ("0", "-1", "abc"):
            with self.subTest(value=value), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    helper.main(["-j", value])
                self.assertEqual(error.exception.code, 2)

    def test_custom_build_directory_is_passed_to_make(self):
        status, _, make = self.run_helper(("--build-dir", "build-odroid-n2"))
        self.assertEqual(status, 0)
        self.assertIn("BUILD_DIR=build-odroid-n2", make.call_args.args[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
