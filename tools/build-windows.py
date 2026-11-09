#!/usr/bin/env python3
"""Cross-build the native Windows x64 application with GCC/MinGW and a UCRT SDK."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shlex
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=ROOT / "build/windows-sdk/ucrt64")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/windows")
    parser.add_argument("-j", type=int, default=4)
    parser.add_argument("--cargo", default=os.getenv("CARGO", "cargo"))
    parser.add_argument("--rust-target", default="x86_64-pc-windows-gnu")
    parser.add_argument("--tests", action="store_true", help="Also build native GTK/video and DVB-T2 tests")
    parser.add_argument("--rebuild", action="store_true", help="Recompile every source, even when timestamps were preserved")
    args = parser.parse_args()
    os.chdir(ROOT)
    sdk = args.sdk.resolve()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    subprocess.run(["bash", str(ROOT / "tools/build-info.sh"),
                    str(build / "generated/linrad_build_info.h")], check=True)
    env = dict(os.environ, PKG_CONFIG_LIBDIR=str(sdk / "lib/pkgconfig"), PKG_CONFIG_PATH="")
    modules = "gtk+-3.0 json-c fftw3f libsamplerate libvlc libavcodec libavformat libavutil libswscale zvbi-0.2 SoapySDR uhd portaudio-2.0 faad2 hidapi LimeSuite libiio".split()
    # MSYS2 calls the samplerate module 'samplerate'.
    modules[3] = "samplerate"
    def pkg(option):
        words = shlex.split(subprocess.check_output(["pkg-config", f"--define-variable=prefix={sdk}", option, *modules], env=env, text=True))
        return [re.sub(r"^(-[IL])(?:[A-Za-z]:[^ ]*)?/ucrt64(?=/|$)", lambda m: m[1] + str(sdk), word) for word in words]
    cflags, libs = pkg("--cflags"), pkg("--libs")
    # New Boost makes System header-only; UHD's .pc still names its old library.
    if not (sdk / "lib/libboost_system.dll.a").exists():
        libs = [word for word in libs if word != "-lboost_system"]
    common = ["-mcrtdll=ucrt", "-O2", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-sign-compare",
              "-Isrc/win32/include", "-Isrc/win32", "-Isrc", "-Icomponents/dvbt/include", "-Icomponents/dvbt2/include",
              "-iquote", "components/tsscatterfix/src", "-Ithird_party/leansdr/src", "-include", "src/win32/platform.h",
              "-DLEANSDR_EXTENSIONS", '-DVERSION="linrad-leansdr-84c59e1"', "-DLINRAD_EMBEDDED_DVBT",
              '-DRBDVBT_VERSION="0.1.5"', *cflags]
    gui = ["linrad_dvb", "iq_file", "decoder_view", "video_view", "scatter_backend", "scatter_view", "ts_info", "ts_info_view",
           "usb_receiver", "usb_demod", "usb_tci", "program_view", "srt_output", "status_http", "dab_view", "dvbt_backend", "dvbt2_backend", "leansdr_backend", "ldpc_pool"]
    cpp = {"dvbt2_backend", "leansdr_backend", "ldpc_pool"}
    gui_sources = [f"src/{name}.{'cc' if name in cpp else 'c'}" for name in gui]
    gui_sources += ["third_party/leansdr/src/apps/leandvb.cc"]
    gui_sources += [str(p) for p in Path("components/dvbt/src").glob("*.c") if p.name != "main.c"]
    gui_sources += [str(p) for p in Path("components/dvbt2/src").rglob("*.cpp") if p.name != "t2gen.cpp"]
    gui_sources += [f"components/tsscatterfix/src/{name}.c" for name in ["recovery", "media", "rank_model", "psi", "ts_packet"]]
    programs = {
        "linrad_dvb": gui_sources,
        "linrad_udp_to_stdout": [f"src/{s}.c" for s in ["linrad_udp_to_stdout", "iq_bandfilter", "iq_monitor", "usb_tap", "iq_record", "iq_output"]],
        "iq_file_to_stdout": [f"src/{s}.c" for s in ["iq_file_to_stdout", "iq_file", "iq_bandfilter", "iq_monitor", "usb_tap"]],
        "sdr_to_stdout": [f"src/{s}.c" for s in ["sdr_to_stdout", "afedri", "lime_rx", "pluto_rx", "iq_output", "iq_record", "iq_bandfilter", "iq_monitor", "usb_tap"]] + ["src/win32/funcube.c"],
        "ldpc_tool": ["third_party/ldpc/ldpc_tool.cc", "third_party/ldpc/tables_handler.cc"],
    }
    dab_sources = [f"components/dab/src/{name}.cpp" for name in
                   "libdxdabsdr dab_detector decoder_backend candidate_fic fic_identity_accumulator channel_filter mot_slideshow dynamic_label dx_msc dx_pad dab_plus_radio".split()]
    dab_dsp = ["components/dvbt2/src/dsp/resampler.cpp", "components/dvbt2/src/dsp/dsp_compat.cpp"]
    programs["dab_worker"] = ["src/dab_worker.cpp", *dab_sources, *dab_dsp]
    if args.tests:
        programs["test_iq_output"] = ["tests/test_iq_output.c", "src/iq_output.c"]
        programs["test_iq_output_backpressure"] = ["tests/test_iq_output_backpressure.c"]
        programs["test_sdr_rx_status"] = ["tests/test_sdr_rx_status.c"]
        programs["test_iq_monitor_async"] = ["tests/test_iq_monitor_async.c", "src/usb_tap.c"]
        programs["monitor_probe"] = ["tests/monitor_probe.c", "src/iq_monitor.c", "src/usb_tap.c"]
        programs["windows_tls"] = ["tests/windows_tls.cpp"]
        programs["windows_gui_smoke"] = ["tests/windows_gui_smoke.c"] + [s for s in gui_sources if s != "src/linrad_dvb.c"]
        programs["dab_gui_smoke"] = ["tests/dab_gui_smoke.c"] + [s for s in gui_sources if s != "src/linrad_dvb.c"]
        programs["dab_fake_source"] = ["tests/dab_fake_source.c"]
        programs["windows_snapshot"] = ["tests/windows_snapshot.c"]
        programs["windows_media"] = ["tests/windows_media.c"]
        programs["test_pluto_rx"] = ["tests/test_pluto_rx.c", "src/pluto_rx.c"]
        programs["test_lime_rx"] = ["tests/test_lime_rx.c", "src/lime_rx.c"]
        for name in ("fec", "l1", "pipeline", "gap_clock", "stage_prof", "clock_correct"):
            programs[f"test_dvbt2_{name}"] = [f"components/dvbt2/tests/test_{name}.cpp"] + [s for s in gui_sources if s.startswith("components/dvbt2/")]
            if name == "gap_clock":
                programs[f"test_dvbt2_{name}"].append("components/dvbt2/src/t2/t2gen.cpp")
        programs["test_dvbt2_ldpc"] = ["tests/dvbt2_ldpc_equivalence.cpp"] + [s for s in gui_sources if s.startswith("components/dvbt2/")]
        for name in "dx_msc dx_pad stream_acquisition libdxdabsdr_tii_synthetic fic_identity_accumulator channel_filter mot_slideshow dynamic_label dab_plus_radio".split():
            programs[f"dab_test_{name}"] = [f"components/dab/tests/{name}_test.cpp", *dab_sources]
    sources = sorted({s for group in programs.values() for s in group} | {"src/win32/platform.c"})
    objects = {}
    for source in sources:
        objects[source] = build / (source + ".o")
        objects[source].parent.mkdir(parents=True, exist_ok=True)

    def compile(source):
        target = objects[source]
        stamp = build / "flags.txt"
        dependencies = target.with_suffix(".d")
        inputs = [Path(source), stamp, Path(__file__)]
        if dependencies.exists():
            inputs += [Path(p) for p in shlex.split(dependencies.read_text().replace("\\\n", " ").split(":", 1)[1])]
        newest = max(p.stat().st_mtime if p.exists() else float("inf") for p in inputs)
        if not args.rebuild and target.exists() and dependencies.exists() and target.stat().st_mtime >= newest:
            return
        compiler = "x86_64-w64-mingw32-g++" if source.endswith((".cc", ".cpp")) else "x86_64-w64-mingw32-gcc"
        flags = ["-std=gnu++20"] if compiler.endswith("g++") else ["-std=gnu11"]
        flags += ["-I" + str(build / "generated")]
        if source.startswith("components/dvbt2/") or source == "src/dvbt2_backend.cc":
            # Match OnAir's Release build and our Linux CXXFLAGS: the OFDM/FEC
            # hot loops need -O3's inlining/vectorization at broadcast rates.
            # Keep runtime AVX2 dispatch and the existing floating-point rules.
            flags += ["-O3"]
        if source == "src/win32/platform.c":
            flags += ["-DLINRAD_WIN32_IMPLEMENTATION"]
        if source == "src/pluto_rx.c":
            flags += ["-DHAVE_LIBIIO", "-DLIBIIO_STATIC"]
        if source == "tests/test_pluto_rx.c":
            flags += ["-DLIBIIO_STATIC"]
        if source == "src/lime_rx.c":
            flags += ["-DHAVE_LIME"]
        if source in dab_sources or source == "src/dab_worker.cpp" or source.startswith("components/dab/tests/"):
            flags += ["-DLINRAD_WIN32_IMPLEMENTATION", "-DHAVE_DAB_RADIO", "-Icomponents/dab/src"]
        if source == "third_party/leansdr/src/apps/leandvb.cc":
            flags += ["-include", "src/win32/lean_compat.h", "-Wno-unused-variable", "-Wno-missing-field-initializers"]
        print(f"Compiling {source}", flush=True)
        subprocess.run([compiler, *common, *flags, "-MMD", "-MF", str(dependencies), "-c", source, "-o", str(target)], check=True)

    flag_text = repr(common)
    stamp = build / "flags.txt"
    if not stamp.exists() or stamp.read_text() != flag_text:
        stamp.write_text(flag_text)
    with ThreadPoolExecutor(max_workers=args.j) as pool:
        list(pool.map(compile, sources))
    rust_dir = build / "rust"
    subprocess.run([args.cargo, "build", "--offline", "--locked", "--release", "--target", args.rust_target,
                    "--manifest-path", "components/dvbt2/rust/Cargo.toml", "--target-dir", str(rust_dir)], check=True)
    rust_lib = rust_dir / args.rust_target / "release/liblinrad_t2_fec.a"
    # GCC 15 emits emulated-TLS destructor registrations. GCC 16's DLL no
    # longer exports that helper. Link the compiler's own implementation into
    # our executables, preserving thread-exit cleanup without mixing DLLs.
    runtime_objects = []
    exports = subprocess.check_output(["x86_64-w64-mingw32-objdump", "-p", str(sdk / "bin/libstdc++-6.dll")], text=True)
    gcc_major = int(subprocess.check_output(["x86_64-w64-mingw32-g++", "-dumpversion"], text=True).split(".")[0])
    if gcc_major < 16 and not re.search(r"\b__cxa_thread_atexit\s*$", exports, re.M):
        archive = subprocess.check_output(["x86_64-w64-mingw32-g++", "-print-file-name=libsupc++.a"], text=True).strip()
        helper = build / "atexit_thread.o"
        helper.write_bytes(subprocess.check_output(["x86_64-w64-mingw32-ar", "p", archive, "atexit_thread.o"]))
        runtime_objects.append(str(helper))
    for name, group in programs.items():
        inputs = [str(objects[s]) for s in group] + [str(objects["src/win32/platform.c"])] + runtime_objects
        if name in ("linrad_dvb", "windows_gui_smoke", "dab_gui_smoke") or name.startswith("test_dvbt2_"):
            inputs.append(str(rust_lib))
        print(f"Linking {name}.exe", flush=True)
        subprocess.run(["x86_64-w64-mingw32-g++", "-mcrtdll=ucrt", "-Wl,--stack,16777216", *inputs, "-o", str(build / f"{name}.exe"),
                        *libs, "-lws2_32", "-luserenv", "-lbcrypt", "-lntdll", "-lwinpthread", "-lsetupapi", "-lole32"], check=True)
    print(f"Built native Windows executables in {build}")


if __name__ == "__main__":
    main()
