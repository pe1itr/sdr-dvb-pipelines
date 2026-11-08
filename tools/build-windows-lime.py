#!/usr/bin/env python3
"""Build the native LimeSDR Mini dependency into the private Windows SDK."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = "23.11.0"
ARCHIVE = f"LimeSuite-v{VERSION}.tar.gz"
URL = f"https://codeload.github.com/myriadrf/LimeSuite/tar.gz/refs/tags/v{VERSION}"
SHA256 = "fd8a448b92bc5ee4012f0ba58785f3c7e0a4d342b24e26275318802dfe00eb33"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=ROOT / "build/windows-sdk")
    parser.add_argument("-j", type=int, default=4)
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    archive = sdk / "packages" / ARCHIVE
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        temporary = archive.with_suffix(".part")
        urllib.request.urlretrieve(URL, temporary)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != SHA256:
            raise RuntimeError("LimeSuite source checksum mismatch")
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError("LimeSuite source checksum mismatch")
    workspace = ROOT / "build/windows-deps-src"
    workspace.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as source:
        source.extractall(workspace, filter="data")
    source = workspace / f"LimeSuite-{VERSION}"

    # Upstream's Windows path targets MSVC. Keep these MinGW adaptations in
    # this reproducible builder, alongside the unmodified source archive.
    def patch(name, before, after):
        path = source / name
        text = path.read_text()
        if before not in text:
            raise RuntimeError(f"LimeSuite patch no longer applies: {name}")
        path.write_text(text.replace(before, after))

    patch("src/Logger.cpp", "#ifdef _MSC_VER", "#ifdef _WIN32")
    patch("src/Logger.cpp", "#define thread_local __declspec( thread )", "// Native C++ thread_local with MinGW.")
    patch("src/Logger.cpp", "<Windows.h>", "<windows.h>")
    patch("src/lms7002m/LMS7002M.cpp", "#ifdef _MSC_VER", "#ifdef _WIN32")
    patch("src/ConnectionFTDI/CMakeLists.txt", "SetupAPI", "setupapi")
    patch("src/threadHelper/threadHelper.cpp", '#include "threadHelper.h"',
          '#include "threadHelper.h"\n#include <pthread.h>')
    patch("src/threadHelper/threadHelper.cpp", "thread->native_handle(), win_priority",
          "pthread_gethandle(thread->native_handle()), win_priority")
    prefix = sdk / "ucrt64"
    build = workspace / "lime-build"
    command = ["cmake", "-S", str(source), "-B", str(build),
               "-DCMAKE_SYSTEM_NAME=Windows", "-DCMAKE_BUILD_TYPE=Release",
               "-DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc",
               "-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++",
               "-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres",
               "-DCMAKE_C_FLAGS=-mcrtdll=ucrt -std=gnu17",
               "-DCMAKE_CXX_FLAGS=-mcrtdll=ucrt -include windows.h -DWIN32_LEAN_AND_MEAN -DNOMINMAX",
               f"-DCMAKE_INSTALL_PREFIX={prefix}", f"-DCMAKE_FIND_ROOT_PATH={prefix}",
               "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY", "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY",
               "-DENABLE_GUI=OFF", "-DENABLE_SOAPY_LMS7=OFF", "-DENABLE_EXAMPLES=OFF",
               "-DENABLE_QUICKTEST=OFF", "-DENABLE_PCIE_XILLYBUS=OFF", "-DENABLE_FX3=OFF",
               "-DFTD3XX_STATIC=OFF"]
    subprocess.run(command, check=True)
    subprocess.run(["cmake", "--build", str(build), "-j", str(args.j)], check=True)
    subprocess.run(["cmake", "--install", str(build)], check=True)
    licenses = prefix / "share/licenses/LimeSuite"
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "COPYING", licenses / "COPYING")
    ftd = (source / "src/ConnectionFTDI/FTD3XXLibrary/FTD3XX.h").read_bytes()
    (licenses / "FTD3XX-license.txt").write_text(ftd.split(b"--*/", 1)[0].decode("cp1252") + "--*/\n")
    manifest = sdk / "source-packages.json"
    packages = json.loads(manifest.read_text()) if manifest.exists() else {}
    packages["LimeSuite"] = {
        "version": VERSION, "url": URL, "archive": ARCHIVE, "sha256": SHA256,
        "builder": "tools/build-windows-lime.py", "configure": command,
        "hardware": "LimeSDR Mini v1.x, FT601/D3XX"}
    manifest.write_text(json.dumps(packages, indent=2) + "\n")


if __name__ == "__main__":
    main()
