#!/usr/bin/env python3
"""Native ODROID-N2/N2+ build for Ubuntu 20.04+ / Debian (aarch64)."""
import argparse
import configparser
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = "build-odroid-n2"
FFMPEG_TAG = "n6.1.6"
FFMPEG_COMMIT = "f1e3a2bf7a2f2cde936d1ed97f09a26853d20125"
PACKAGES = (
    "build-essential", "pkg-config", "python3", "libgtk-3-dev", "libglib2.0-dev",
    "libjson-c-dev", "libfftw3-dev", "libsoapysdr-dev", "libuhd-dev", "libboost-system-dev",
    "libasound2-dev", "libsamplerate0-dev", "libvlc-dev", "libavcodec-dev",
    "libavformat-dev", "libavutil-dev", "libswscale-dev", "libzvbi-dev",
    "ffmpeg", "soapysdr-tools", "soapysdr-module-rtlsdr", "rtl-sdr",
)
FFMPEG_PACKAGES = ("git", "ca-certificates", "libsrt-dev", "libssl-dev", "libx264-dev", "nasm")


def os_release(path=Path("/etc/os-release")):
    values = {}
    for line in path.read_text().splitlines():
        if "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            values[key] = shlex.split(value)[0] if value else ""
    return values


def require_platform():
    if sys.platform != "linux" or platform.machine() not in ("aarch64", "arm64") or struct.calcsize("P") != 8:
        raise ValueError("Run this script on the ODROID with a 64-bit aarch64 Linux userspace; it is not a cross-compiler.")
    release = os_release()
    family = {release.get("ID", ""), *release.get("ID_LIKE", "").split()}
    if not family.intersection({"ubuntu", "debian"}):
        raise ValueError("This package installer supports Ubuntu/Debian and their Armbian variants.")
    print("System:", release.get("PRETTY_NAME", "Linux"), platform.machine(), flush=True)


def build_environment():
    env = os.environ.copy()
    # ARMv8-A runs on both clusters. Do not use x86 flags or -march=native.
    tuning = "-march=armv8-a -mtune=cortex-a73.cortex-a53"
    for key, default in (("CFLAGS", "-O2 -Wall -Wextra -std=c11"),
                         ("CXXFLAGS", "-O3 -Wall -Wextra -std=gnu++17")):
        env[key] = env.get(key, default) + " " + tuning
    return env


def run(command, **kwargs):
    print("+", shlex.join(str(x) for x in command), flush=True)
    return subprocess.run(command, check=True, **kwargs)


def install_dependencies(build_ffmpeg=False):
    if not shutil.which("apt-get"):
        raise ValueError("apt-get is unavailable")
    prefix = [] if os.geteuid() == 0 else ["sudo"]
    run([*prefix, "apt-get", "update"])
    # Use the system's repositories; do not replace its kernel, GLib or FFmpeg
    # with a PPA. Package postinst scripts install the usual RTL-SDR udev rules.
    packages = PACKAGES + (FFMPEG_PACKAGES if build_ffmpeg else ())
    run([*prefix, "apt-get", "install", "-y", "--no-install-recommends", *packages])


def local_ffmpeg(jobs):
    """Build only the formats/codecs needed by this repeater, in its own prefix."""
    source = ROOT / BUILD_DIR / "ffmpeg-src"
    prefix = ROOT / BUILD_DIR / "ffmpeg"
    source.parent.mkdir(parents=True, exist_ok=True)
    if not source.exists():
        run(["git", "clone", "--depth", "1", "--branch", FFMPEG_TAG,
             "https://github.com/FFmpeg/FFmpeg.git", str(source)])
    commit = run(["git", "-C", str(source), "rev-parse", "HEAD"], stdout=subprocess.PIPE, text=True).stdout.strip()
    if commit != FFMPEG_COMMIT:
        raise ValueError("Unexpected FFmpeg checkout in " + str(source) + "; expected " + FFMPEG_COMMIT)
    run([str(source / "configure"), "--prefix=" + str(prefix), "--disable-autodetect",
         "--disable-doc", "--disable-debug", "--disable-ffplay", "--disable-shared", "--enable-static",
         "--disable-everything", "--enable-gpl", "--enable-libx264", "--enable-libsrt",
         "--enable-ffmpeg", "--enable-ffprobe", "--enable-protocol=file,pipe,libsrt,udp,tcp",
         "--enable-demuxer=mpegts,mpegtsraw", "--enable-muxer=mpegts,null,rawvideo", "--enable-indev=lavfi",
         "--enable-decoder=h264,hevc,mpeg2video,mpeg4,aac,aac_latm,mp2,mp3,ac3,eac3,rawvideo,pcm_s16le,pcm_f32le",
         "--enable-encoder=libx264,aac,rawvideo",
         "--enable-parser=h264,hevc,mpegvideo,mpeg4video,aac,aac_latm,mpegaudio,ac3",
         "--enable-filter=bwdif,scale,setsar,pad,fps,format,aformat,aresample,anull,null,buffer,buffersink,abuffer,abuffersink,testsrc2,sine,color",
         "--enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata"], cwd=source)
    run(["make", "-j" + str(jobs)], cwd=source)
    run(["make", "install"], cwd=source)
    return str(prefix / "bin/ffmpeg")


def check_repeater(ffmpeg):
    def output(command):
        return run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60).stdout
    protocols = output([ffmpeg, "-hide_banner", "-protocols"])
    if "srt" not in protocols.partition("Output:")[2].split():
        raise ValueError("FFmpeg has no SRT output. Supply an SRT-enabled binary with --ffmpeg /path/to/ffmpeg.")
    encoders = output([ffmpeg, "-hide_banner", "-encoders"])
    for name in ("libx264", "aac"):
        if not re.search(r"^\s*[A-Z.]+\s+" + name + r"\s", encoders, re.M):
            raise ValueError("FFmpeg is missing encoder " + name)
    drivers = output(["SoapySDRUtil", "--info"])
    if not re.search(r"Available factories[^\n]*\brtlsdr\b", drivers):
        raise ValueError("SoapyRTLSDR is unavailable; install soapysdr-module-rtlsdr.")
    print("SRT, H.264, AAC and the RTL-SDR driver are available (no radio or network connection opened).", flush=True)


def write_config(path, ffmpeg):
    config = configparser.ConfigParser(interpolation=None)
    config.read(ROOT / "linrad-dvb.example.ini")
    for key, name in (("sdr", "sdr_to_stdout"), ("linrad", "linrad_udp_to_stdout"), ("iq_file", "iq_file_to_stdout")):
        config["paths"][key] = str(ROOT / BUILD_DIR / name)
    config["paths"]["ffmpeg"] = str(Path(shutil.which(ffmpeg) or ffmpeg).resolve())
    config["session"].update(source="rtlsdr", standard="DVB-T", sr="250000", fec="1/2", gi="auto",
                             scatterfix="false", renderer="srt")
    config["rtlsdr"]["frequency"] = "0"  # Deliberately invalid until the operator selects the actual RF frequency.
    config["srt"].update(url="srt://44.137.26.85:4001", x264_preset="ultrafast", video_threads="3", filter_threads="1")
    try:
        with path.open("x") as output:
            output.write("# ODROID-N2 PI6EHV template: set [rtlsdr] frequency (Hz) before reception.\n"
                         "# [session] sr=250000, fec=1/2 and gi=auto are EXAMPLES; match your transmitter.\n"
                         "# frequency=0 intentionally prevents starting with an assumed RF frequency.\n")
            config.write(output)
    except FileExistsError:
        print("Preserving existing configuration:", path)
        print("Check its [paths] entries against", ROOT / BUILD_DIR, "and ffmpeg=" + config["paths"]["ffmpeg"])
    else:
        print("Created configuration:", path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-deps", action="store_true", help="install Ubuntu/Debian packages using sudo apt-get")
    parser.add_argument("--build-ffmpeg", action="store_true", help="build a local FFmpeg with SRT (needed with stock Ubuntu 20.04)")
    parser.add_argument("--check", action="store_true", help="check only; no package installation, compilation or configuration writes")
    parser.add_argument("-j", "--jobs", type=int, default=1, help="parallel compiler jobs (default 1 to limit RAM use)")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="SRT-capable FFmpeg executable")
    parser.add_argument("--config", type=Path, default=ROOT / "pi6ehv-odroid.ini", help="create this template after building; never overwrite")
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.check and (args.install_deps or args.build_ffmpeg):
        parser.error("--check cannot be combined with --install-deps or --build-ffmpeg")
    if args.build_ffmpeg and args.ffmpeg != "ffmpeg":
        parser.error("Choose --build-ffmpeg or --ffmpeg PATH")
    try:
        require_platform()
        if args.install_deps:
            install_dependencies(args.build_ffmpeg)
        env = build_environment()
        command = [sys.executable, str(ROOT / "tools/build.py"), "--build-dir", BUILD_DIR, "-j", str(args.jobs)]
        # Compile/link probes first; require the repeater runtime before a long build.
        run([*command, "--check"], env=env)
        if args.build_ffmpeg:
            args.ffmpeg = local_ffmpeg(args.jobs)
        elif args.ffmpeg == "ffmpeg" and (ROOT / BUILD_DIR / "ffmpeg/bin/ffmpeg").is_file():
            args.ffmpeg = str(ROOT / BUILD_DIR / "ffmpeg/bin/ffmpeg")
        check_repeater(args.ffmpeg)
        if args.check:
            return 0
        run(command, env=env)
        path = args.config.resolve()
        write_config(path, args.ffmpeg)
        print("\nSet the RF frequency and DVB-T parameters in the configuration, then start:")
        print(shlex.join([str(ROOT / BUILD_DIR / "linrad_dvb"), "--config", str(path), "--headless"]))
        print("1080p25 software encoding uses ultrafast / 3 video threads / 1 filter thread.")
        print("Measure sustained reception on the N2; this build does not prove realtime encoding capacity.")
        return 0
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        print("ERROR:", exc, file=sys.stderr)
        print("For apt package lookup failures, check that Ubuntu's universe component is enabled.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
