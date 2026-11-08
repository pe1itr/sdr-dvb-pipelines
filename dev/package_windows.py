#!/usr/bin/env python3
"""Assemble a relocatable Windows ZIP and verify its complete DLL import tree."""
import argparse
import hashlib
import json
from functools import lru_cache
from pathlib import Path
import pefile
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SYSTEM_DLLS = set("advapi32 avicap32 avrt bcrypt bcryptprimitives cabinet cfgmgr32 clbcatq comctl32 comdlg32 crypt32 cryptbase cryptnet cryptsp d2d1 d3d9 d3d11 d3d12 d3dcompiler_47 dbghelp dnsapi dsound dwmapi dwrite dxgi dxva2 evr gdi32 gdiplus glu32 hid imm32 iphlpapi kernel32 ksuser mf mfcore mfplat mfreadwrite mfuuid mpr msacm32 msvcrt mswsock ncrypt netapi32 normaliz ntdll ole32 oleacc oleaut32 opengl32 pdh powrprof propsys psapi quartz rpcrt4 secur32 setupapi shell32 shlwapi strmiids user32 userenv usp10 uxtheme ucrtbase version vfw32 winhttp wininet winmm winscard winspool wintrust wldap32 ws2_32 wtsapi32".split())
SYSTEM_DLLS.update("msimg32 msdmo winusb authz cryptui ktmw32 rasapi32 usp10 imagehlp msvfw32 wsock32".split())


@lru_cache(maxsize=None)
def pe_symbols(path):
    with pefile.PE(str(path), fast_load=True) as pe:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY[key] for key in
                                  ["IMAGE_DIRECTORY_ENTRY_IMPORT", "IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT", "IMAGE_DIRECTORY_ENTRY_EXPORT"]])
        imports = {}
        for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []) + getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
            imports.setdefault(entry.dll.decode("ascii"), set()).update(
                symbol.name.decode("ascii") if symbol.name else symbol.ordinal for symbol in entry.imports)
        exports = set()
        for symbol in getattr(getattr(pe, "DIRECTORY_ENTRY_EXPORT", None), "symbols", []):
            if symbol.address:
                exports.add(symbol.ordinal)
                if symbol.name:
                    exports.add(symbol.name.decode("ascii"))
        return imports, exports


def system_dll(name):
    return name.startswith(("api-ms-win-", "ext-ms-win-")) or Path(name).stem in SYSTEM_DLLS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=ROOT / "build/windows-sdk/ucrt64")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/windows")
    parser.add_argument("--stage-only", action="store_true")
    parser.add_argument("--b210-images", type=Path, help="Bundle this receiver's own FPGA/firmware files and select them in the INI")
    args = parser.parse_args()
    sdk, build = args.sdk.resolve(), args.build_dir.resolve()
    stage = ROOT / "build/linrad_dvb-windows-x64"
    # A staging directory is generated output, never an installed user profile.
    if stage.is_symlink():
        raise RuntimeError(f"Refusing symlink staging directory: {stage}")
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True, exist_ok=True)
    def copy(source, relative):
        target = stage / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        return target
    binaries = []
    for name in ["linrad_dvb", "linrad_udp_to_stdout", "iq_file_to_stdout", "sdr_to_stdout", "ldpc_tool", "dab_worker"]:
        binaries.append(copy(build / f"{name}.exe", f"{name}.exe"))
    for name in ["gspawn-win64-helper.exe", "gspawn-win64-helper-console.exe", "ffmpeg.exe", "ffplay.exe"]:
        binaries.append(copy(sdk / "bin" / name, name))
    # Qt is only needed for VLC's standalone UI. The application embeds libVLC
    # with video callbacks; don't bundle an unrelated second GUI.
    for folder in ["access", "audio_filter", "audio_mixer", "audio_output", "codec", "demux", "logger", "misc", "packetizer", "stream_filter", "video_chroma", "video_filter", "video_output", "text_renderer"]:
        for plugin in (sdk / "lib/vlc/plugins" / folder).glob("*.dll"):
            binaries.append(copy(plugin, Path("plugins") / folder / plugin.name))
    for module in (sdk / "lib/SoapySDR/modules0.8").glob("*.dll"):
        binaries.append(copy(module, Path("lib/SoapySDR/modules0.8") / module.name))
    pixbuf = Path("lib/gdk-pixbuf-2.0/2.10.0")
    # GTK's embedded Adwaita symbolic assets need librsvg as well as the
    # slideshow's PNG/JPEG loaders. Include it in the DLL dependency walk.
    for name in ("libpixbufloader-png.dll", "libpixbufloader-jpeg.dll", "pixbufloader_svg.dll"):
        relative = pixbuf / "loaders" / name
        binaries.append(copy(sdk / relative, relative))
    copy(ROOT / "dev/windows-pixbuf-loaders.cache", pixbuf / "loaders.cache")
    for folder in ["etc/fonts", "share/fonts", "share/glib-2.0/schemas", "share/themes", "share/licenses"]:
        if (sdk / folder).is_dir():
            shutil.copytree(sdk / folder, stage / folder, dirs_exist_ok=True)
    (stage / "etc/fonts/fonts.conf").write_text('''<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir prefix="relative">../../share/fonts</dir>
  <dir>WINDOWSFONTDIR</dir>
  <dir>WINDOWSUSERFONTDIR</dir>
  <cachedir>LOCAL_APPDATA_FONTCONFIG_CACHE</cachedir>
  <alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias>
  <alias><family>monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>
</fontconfig>
''')
    available = {p.name.lower(): p for p in (sdk / "bin").glob("*.dll")}
    checked = set()
    manifest = {}
    missing = []
    while binaries:
        binary = binaries.pop()
        if binary in checked:
            continue
        checked.add(binary)
        dependencies, _ = pe_symbols(binary)
        manifest[str(binary.relative_to(stage))] = list(dependencies)
        for dependency in dependencies:
            lower = dependency.lower()
            if system_dll(lower):
                continue
            if lower not in available:
                missing.append(f"{dependency} (required by {binary.name})")
                continue
            target = stage / available[lower].name
            if target not in checked:
                binaries.append(copy(available[lower], target.name))
    if missing:
        raise RuntimeError("Missing DLLs: " + ", ".join(missing))
    for binary in checked:
        for dependency, required in pe_symbols(binary)[0].items():
            lower = dependency.lower()
            if system_dll(lower):
                continue
            exported = pe_symbols(stage / available[lower].name)[1]
            missing.extend(f"{binary.name}: {dependency}!{symbol}" for symbol in required - exported)
    if missing:
        raise RuntimeError("Missing DLL exports:\n" + "\n".join(sorted(missing)))
    (stage / "dll-imports.json").write_text(json.dumps(manifest, indent=2) + "\n")
    copy(sdk.parent / "packages.json", "dependency-packages.json")
    source_manifest = sdk.parent / "source-packages.json"
    copy(source_manifest, "source-packages.json")
    for package in json.loads(source_manifest.read_text()).values():
        archive = sdk.parent / "packages" / package["archive"]
        if hashlib.sha256(archive.read_bytes()).hexdigest() != package["sha256"]:
            raise RuntimeError(f"Source checksum mismatch: {archive}")
        copy(archive, Path("source") / archive.name)
    for name in ["LICENSE", "NOTICE"]:
        copy(ROOT / name, name)
    for name in ["credits.md", "windows.md"]:
        copy(ROOT / "docs" / name, Path("docs") / name)
    for path in (ROOT / "docs/licenses").glob("*"):
        copy(path, Path("docs/licenses") / path.name)
    for folder, name in [("third_party/leansdr", "LICENSE.txt"), ("third_party/ldpc", "LICENSE"),
                         ("components/dvbt", "LICENSE"), ("components/dvbt2", "LICENSE"), ("components/tsscatterfix", "LICENSE")]:
        copy(ROOT / folder / name, Path("licenses") / (Path(folder).name + "-LICENSE.txt"))
    diagnostic = '@echo off\nset LINRAD_DIAGNOSTIC_LOG=1\ncd /d "%~dp0"\necho Log: windows-diagnose.log\n"%~dp0linrad_dvb.exe" > "%~dp0windows-diagnose.log" 2>&1\npause\n'
    (stage / "start-met-log.cmd").write_bytes(diagnostic.replace("\n", "\r\n").encode("ascii"))
    profile = (ROOT / "dev/start-dvbt2-profile.cmd").read_text()
    (stage / "start-dvbt2-profile.cmd").write_bytes(profile.replace("\n", "\r\n").encode("ascii"))
    config = (ROOT / "linrad-dvb.example.ini").read_text()
    for name in ["sdr_to_stdout", "linrad_udp_to_stdout", "iq_file_to_stdout", "dab_worker"]:
        config = config.replace("build/" + name, "./" + name + ".exe")
    config = config.replace("gpu_program=/usr/bin/ffplay", "gpu_program=ffplay.exe")
    config = config.replace("cvlc=cvlc", "cvlc=vlc.exe").replace("vlc=vlc\n", "vlc=vlc.exe\n")
    if args.b210_images:
        images = {}
        for name in ("usrp_b210_fpga.bin", "usrp_b200_fw.hex"):
            target = copy(args.b210_images / name, Path("uhd-images/kintex7") / name)
            images[name] = hashlib.sha256(target.read_bytes()).hexdigest()
        (stage / "uhd-images/kintex7/SHA256SUMS").write_text(
            "".join(f"{digest}  {name}\n" for name, digest in images.items()))
        before, section = config.split("[b210]", 1)
        b210, after = section.split("[rtlsdr]", 1)
        b210 = b210.replace("args=\n", "args=type=b200\n")
        b210 = b210.replace("images=\n", "images=./uhd-images/kintex7\n")
        config = before + "[b210]" + b210 + "[rtlsdr]" + after
        (stage / "uhd-images/kintex7/LEESMIJ.txt").write_text(
            "Deze bestanden zijn exact overgenomen uit de werkende Linux-configuratie van de eigenaar.\n"
            "Bestemd voor diens B210-compatibele Kintex-7-ontvanger, niet voor een originele Ettus B210.\n"
            "Device arguments: type=b200. Bewaar wijzigingen in de applicatie met W.\n"
            "De SHA-256-controles staan in SHA256SUMS.\n")
    (stage / "linrad-dvb.example.ini").write_text(config)
    # Ship defaults only: extracting an update must never replace user settings.
    # The Windows application copies the example on its first start if needed.
    (stage / "LEESMIJ.txt").write_text(
        "Linrad DVB - native Windows x64 (experimentele port)\n\n"
        "Pak de volledige ZIP uit in een schrijfbare map. Start linrad_dvb.exe.\n"
        "WSL, MSYS2 en een compiler zijn niet nodig. DLL's en plugins moeten bij de EXE blijven.\n"
        "Kies Input, Mode en Start SDR. W bewaart instellingen in linrad-dvb.ini.\n"
        "De eerste start maakt linrad-dvb.ini uit het voorbeeld; updates behouden een bestaande INI.\n"
        "Zie docs/windows.md voor drivers, beperkingen en teststatus.\n")
    print(f"Verified {len(checked)} PE files; runtime staged in {stage}")
    if args.stage_only:
        return
    subprocess.run(["python3", str(ROOT / "dev/dist.py")], check=True)
    copy(ROOT / "dist/linrad_dvb-source.zip", "source/linrad_dvb-source.zip")
    destination = ROOT / "dist/linrad_dvb-windows-x64.zip"
    with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
        archive = Path(temporary) / destination.name
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as output:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    output.write(path, Path(stage.name) / path.relative_to(stage))
        archive.replace(destination)
    digest = hashlib.sha256(destination.read_bytes()).hexdigest()
    destination.with_suffix(".zip.sha256").write_text(f"{digest}  {destination.name}\n")
    print(f"Created {destination} ({destination.stat().st_size / 1024**2:.1f} MiB), SHA256 {digest}")


if __name__ == "__main__":
    main()
