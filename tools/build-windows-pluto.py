#!/usr/bin/env python3
"""Build pinned libiio 0.26 (USB and network) for the native Pluto RX backend."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = 'libiio-v0.26.tar.gz'
URL = 'https://codeload.github.com/analogdevicesinc/libiio/tar.gz/refs/tags/v0.26'
SHA256 = 'fb445fb860ef1248759f45d4273a4eff360534480ec87af64c6b8db3b99be7e5'
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=ROOT/'build/windows-sdk')
    parser.add_argument('-j', type=int, default=4)
    args = parser.parse_args()
    sdk = args.sdk.resolve(); prefix = sdk/'ucrt64'
    archive = sdk/'packages'/ARCHIVE
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        temporary = archive.with_suffix('.part')
        urllib.request.urlretrieve(URL, temporary)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != SHA256:
            raise RuntimeError('libiio checksum mismatch')
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('libiio checksum mismatch')
    workspace = ROOT/'build/windows-deps-src'
    workspace.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as source: source.extractall(workspace, filter='data')
    source = workspace/'libiio-0.26'
    utilities = source/'utilities.c'
    utilities.write_text(utilities.read_text().replace('<Windows.h>', '<windows.h>'))
    build = workspace/'pluto-build'
    env = dict(os.environ, PKG_CONFIG_PATH='', PKG_CONFIG_LIBDIR=str(prefix/'lib/pkgconfig'), PKG_CONFIG_SYSROOT_DIR=str(sdk))
    command = ['cmake', '-S', str(source), '-B', str(build),
        '-DCMAKE_SYSTEM_NAME=Windows', '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc',
        '-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres', '-DCMAKE_C_FLAGS=-mcrtdll=ucrt -std=gnu17',
        f'-DCMAKE_INSTALL_PREFIX={prefix}', f'-DCMAKE_FIND_ROOT_PATH={prefix}',
        '-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY', '-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY',
        '-DWITH_TESTS=OFF', '-DWITH_EXAMPLES=OFF', '-DHAVE_DNS_SD=OFF',
        '-DWITH_LOCAL_BACKEND=OFF', '-DWITH_IIOD=OFF', '-DWITH_USB_BACKEND=ON', '-DWITH_NETWORK_BACKEND=ON',
        f'-DLIBXML2_LIBRARY={prefix}/lib/libxml2.dll.a', f'-DLIBXML2_INCLUDE_DIR={prefix}/include/libxml2']
    subprocess.run(command, env=env, check=True)
    subprocess.run(['cmake', '--build', str(build), '-j', str(args.j)], check=True)
    subprocess.run(['cmake', '--install', str(build)], check=True)
    licenses = prefix/'share/licenses/libiio'; licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source/'COPYING.txt', licenses/'COPYING.txt')
    manifest = sdk/'source-packages.json'
    packages = json.loads(manifest.read_text()) if manifest.exists() else {}
    packages['libiio'] = dict(version='0.26', url=URL, archive=ARCHIVE, sha256=SHA256,
        builder='tools/build-windows-pluto.py', configure=command, hardware='Pluto / AD9361 RX1; USB or network URI')
    manifest.write_text(json.dumps(packages, indent=2)+'\n')
if __name__ == '__main__': main()
