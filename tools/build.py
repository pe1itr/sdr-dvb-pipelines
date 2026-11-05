#!/usr/bin/env python3
"""Check dependencies for the complete Linux build, then run make all."""

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
# Keep in sync with the libraries linked by Makefile's `all` target.
# These are mandatory even when the corresponding GUI feature is not used.
LIBRARIES = (
    ("gtk+-3.0", "gtk/gtk.h", "GTK 3 GUI"),
    ("glib-2.0", "glib.h", "GLib"),
    ("json-c", "json-c/json.h", "IQ metadata"),
    ("fftw3f", "fftw3.h", "single-precision FFTW3"),
    ("SoapySDR", "SoapySDR/Device.h", "SDR input"),
    ("uhd", "uhd.h", "USRP/B210 input"),
    ("alsa", "alsa/asoundlib.h", "audio and FUNcube input"),
    ("samplerate", "samplerate.h", "audio resampling"),
    ("libvlc", "vlc/vlc.h", "embedded player"),
    ("libavcodec", "libavcodec/avcodec.h", "Scatterfix codecs"),
    ("libavformat", "libavformat/avformat.h", "Scatterfix media"),
    ("libavutil", "libavutil/avutil.h", "Scatterfix utilities"),
    ("libswscale", "libswscale/swscale.h", "Scatterfix image scaling"),
    ("zvbi-0.2", "libzvbi.h", "teletext"),
)
OPTIONAL_PROGRAMS = (
    ("vlc", "external VLC renderer"),
    ("cvlc", "command-line VLC renderer"),
    ("ffplay", "external FFplay renderer"),
    ("direwolf", "external packet-radio decoder"),
)


class Checker:
    def __init__(self):
        self.errors = 0
        self.warnings = 0

    def report(self, level, message):
        if level == "ERROR":
            self.errors += 1
        elif level == "WARNING":
            self.warnings += 1
        print(f"[{level}] {message}", flush=True)

    def command(self, argv):
        try:
            result = subprocess.run(argv, cwd=ROOT, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=60)
        except (OSError, subprocess.TimeoutExpired) as exc:
            return False, str(exc)
        return result.returncode == 0, (result.stdout + result.stderr).strip()

    def compile(self, directory, compiler, flags, source, suffix, link_flags):
        path = directory / f"probe.{suffix}"
        path.write_text(source)
        return self.command(compiler + flags + [str(path), "-o", str(directory / "probe")]
                            + link_flags)

    def optional(self):
        if shutil.which("pkg-config"):
            ok, _ = self.command(["pkg-config", "--print-errors", "--exists", "LimeSuite"])
            self.report("OK" if ok else "WARNING", "LimeSuite: native LimeSDR Mini RX enabled" if ok else
                        "LimeSuite development files missing; LimeSDR Mini RX unavailable. "
                        "Set PKG_CONFIG_PATH for a private installation, then rebuild.")
        for program, purpose in OPTIONAL_PROGRAMS:
            if shutil.which(program):
                self.report("OK", f"{program}: {purpose}")
            else:
                self.report("WARNING", f"{program} missing; unavailable: {purpose}.")

        # --info loads driver modules but does not enumerate or open radios.
        if not shutil.which("SoapySDRUtil"):
            self.report("WARNING", "SoapySDRUtil missing; cannot check optional Soapy drivers.")
            return
        ok, output = self.command(["SoapySDRUtil", "--info"])
        factories = re.search(r"Available factories[^\n]*", output)
        if not ok or factories is None:
            self.report("WARNING", f"Cannot determine available Soapy drivers:\n{output}")
            return
        for driver, device in (("hackrf", "HackRF"), ("rfspace", "SDR-IQ"),
                               ("rtlsdr", "RTL-SDR"), ("sdrplay", "SDRplay RSP1A/B (API v3)"),
                               ("airspyhf", "Airspy HF+ Dual Port"), ("airspy", "Airspy R2")):
            if re.search(r"\b" + driver + r"\b", factories.group()):
                self.report("OK", f"Soapy driver {driver}: {device}")
            else:
                self.report("WARNING", f"Soapy driver {driver} missing; {device} input unavailable.")


def positive_int(value):
    try:
        number = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError("must be a positive integer") from None
    if number < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="check only; do not build")
    parser.add_argument("-j", "--jobs", type=positive_int, default=1,
                        help="parallel make jobs (default: 1)")
    args = parser.parse_args(argv)
    checker = Checker()
    if not sys.platform.startswith("linux"):
        checker.report("ERROR", "The current Makefile requires Linux (ALSA and hidraw).")
        return 1

    defaults = {"CC": "cc", "CXX": "g++", "CPPFLAGS": "",
                "CFLAGS": "-O2 -Wall -Wextra -std=c11",
                "CXXFLAGS": "-O3 -Wall -Wextra -std=gnu++17",
                "LDFLAGS": "", "LDLIBS": ""}
    settings = {name: os.environ.get(name, value) for name, value in defaults.items()}
    try:
        words = {name: shlex.split(value) for name, value in settings.items()}
    except ValueError as exc:
        checker.report("ERROR", f"Invalid compiler/flag environment setting: {exc}")
        return 1

    ready = {}
    for name, command in (("make", ["make"]), ("pkg-config", ["pkg-config"]),
                          ("CC", words["CC"]), ("CXX", words["CXX"])):
        ready[name] = bool(command and shutil.which(command[0]))
        checker.report("OK" if ready[name] else "ERROR",
                       f"{name}: {shlex.join(command) or '(empty)'}"
                       + ("" if ready[name] else " — executable missing"))

    for source in ("third_party/leansdr/src/apps/leandvb.cc",
                   "third_party/ldpc/ldpc_tool.cc", "third_party/rbdvbt/src/dvbt_outer.c",
                   "third_party/tsscatterfix/src/recovery.c"):
        if not (ROOT / source).is_file():
            checker.report("ERROR", f"Bundled source missing: {source}; restore the complete checkout.")

    with tempfile.TemporaryDirectory(prefix="sdr-build-check-") as temp:
        directory = Path(temp)
        link_flags = words["LDFLAGS"] + words["LDLIBS"]
        probes = (
            ("CC", "CFLAGS", "c", "#include <linux/hidraw.h>\n#include <pthread.h>\n"
             "#include <math.h>\nint main(void) { return 0; }\n", ["-pthread", "-lm"]),
            ("CXX", "CXXFLAGS", "cc", "#include <thread>\n#include <vector>\n"
             "static_assert(__cplusplus >= 201703L, \"C++17 required\");\n"
             "int main() { std::thread t([] {}); t.join(); }\n", ["-pthread"]),
        )
        for compiler, flags, suffix, source, extra in probes:
            if ready[compiler]:
                ok, detail = checker.compile(directory, words[compiler],
                                             words["CPPFLAGS"] + words[flags], source,
                                             suffix, link_flags + extra)
                ready[compiler] = ok
                checker.report("OK" if ok else "ERROR",
                               f"{compiler}: compile/link check" + ("" if ok else f"\n{detail}"))

        if ready["pkg-config"]:
            for module, header, purpose in LIBRARIES:
                ok, detail = checker.command(["pkg-config", "--print-errors", "--exists", module])
                if not ok:
                    checker.report("ERROR", f"{module} ({purpose}): development package missing "
                                   f"or not found in PKG_CONFIG_PATH.\n{detail}")
                    continue
                if not ready["CC"]:
                    continue
                c_ok, cflags = checker.command(["pkg-config", "--cflags", module])
                l_ok, libs = checker.command(["pkg-config", "--libs", module])
                if not c_ok or not l_ok:
                    checker.report("ERROR", f"{module}: cannot read build flags.\n{cflags}\n{libs}")
                    continue
                # Match the application's POSIX feature environment. VLC
                # also expects ssize_t to be declared before its public header.
                source = ("#ifndef _POSIX_C_SOURCE\n#define _POSIX_C_SOURCE 200809L\n#endif\n"
                          f"#include <sys/types.h>\n#include <{header}>\n"
                          "int main(void) { return 0; }\n")
                ok, detail = checker.compile(
                    directory, words["CC"], words["CPPFLAGS"] + words["CFLAGS"]
                    + shlex.split(cflags), source, "c",
                    words["LDFLAGS"] + shlex.split(libs) + words["LDLIBS"])
                checker.report("OK" if ok else "ERROR", f"{module} ({purpose}): headers/link check"
                               + ("" if ok else f" failed\n{detail}"))

    checker.optional()
    print(f"\nDependency check: {checker.errors} error(s), {checker.warnings} warning(s).", flush=True)
    if checker.errors:
        print("Build blocked. Install the required development packages and rerun this helper.")
        return 1
    if args.check:
        return 0
    # Pass the checked settings explicitly so make uses the same compilers/flags.
    command = ["make", "-C", str(ROOT), f"-j{args.jobs}", "all"]
    command += [f"{name}={value}" for name, value in settings.items()]
    print("Building: make all", flush=True)
    return subprocess.call(command, cwd=ROOT)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
