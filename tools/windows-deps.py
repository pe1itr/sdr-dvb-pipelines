#!/usr/bin/env python3
"""Fetch a private UCRT64 SDK from MSYS2; never install into the host system.

The package database supplies SHA-256 hashes. Keep packages.json with the
resulting distribution to record the exact binary/source package versions.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import urllib.request

BASE = "https://repo.msys2.org/mingw/ucrt64/"
PREFIX = "mingw-w64-ucrt-x86_64-"
ROOTS = "gtk3 json-c fftw libsamplerate ffmpeg vlc zvbi soapysdr soapyhackrf soapyrtlsdr libuhd portaudio faad2 hidapi ttf-dejavu".split()


def fetch(url, path, checksum=None):
    if not path.exists() or (checksum and hashlib.sha256(path.read_bytes()).hexdigest() != checksum):
        temporary = path.with_suffix(path.suffix + ".part")
        urllib.request.urlretrieve(url, temporary)
        if checksum and hashlib.sha256(temporary.read_bytes()).hexdigest() != checksum:
            raise ValueError(f"SHA-256 mismatch: {url}")
        temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--database", type=Path)
    args = parser.parse_args()
    dest = args.destination.resolve()
    cache = dest / "packages"
    cache.mkdir(parents=True, exist_ok=True)
    database = args.database or cache / "ucrt64.db"
    if not args.database:
        fetch(BASE + "ucrt64.db", database)
    packages, providers = {}, {}
    with tarfile.open(database) as archive:
        for member in archive:
            if not member.name.endswith("/desc"):
                continue
            fields = {}
            for block in archive.extractfile(member).read().decode().strip().split("\n\n"):
                key, *values = block.splitlines()
                fields[key.strip("%")] = values
            name = fields["NAME"][0]
            packages[name] = fields
            for provided in fields.get("PROVIDES", []):
                providers[re.split(r"[<>=]", provided)[0]] = name
    selected = {}

    def select(name):
        name = re.split(r"[<>=]", name)[0]
        name = name if name in packages else providers.get(name, name)
        if name in selected:
            return
        package = packages[name]
        selected[name] = package
        for dependency in package.get("DEPENDS", []):
            select(dependency)

    for root in ROOTS:
        select(PREFIX + root)
    print(f"Downloading {len(selected)} Windows SDK packages", flush=True)

    def download(package):
        filename = package["FILENAME"][0]
        fetch(BASE + filename, cache / filename, package["SHA256SUM"][0])
        print(filename, flush=True)

    with ThreadPoolExecutor(max_workers=6) as pool:
        list(pool.map(download, selected.values()))
    for package in selected.values():
        # This is an SDK/runtime, not a full MSYS2 installation. In particular
        # Python's standard library and Qt developer files are not needed.
        subprocess.run(["tar", "--zstd", "-xf", str(cache / package["FILENAME"][0]),
                        "-C", str(dest), "--exclude=*.a", "--exclude=ucrt64/lib/python*",
                        "--exclude=ucrt64/share/doc", "--exclude=ucrt64/share/gtk-doc",
                        "--exclude=ucrt64/share/locale", "--exclude=ucrt64/share/icons",
                        "--exclude=ucrt64/include/qt6", "--exclude=ucrt64/lib/cmake"], check=True)
        # Import libraries are needed; static archives are deliberately omitted.
        subprocess.run(["tar", "--zstd", "-xf", str(cache / package["FILENAME"][0]),
                        "-C", str(dest), "--wildcards", "--no-anchored", "*.dll.a"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    (dest / "packages.json").write_text(json.dumps(selected, indent=2) + "\n")


if __name__ == "__main__":
    main()
