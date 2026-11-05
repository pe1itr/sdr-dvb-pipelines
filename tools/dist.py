#!/usr/bin/env python3
"""Package the current Linrad DVB sources without requiring Git or a build."""

from pathlib import Path
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parent.parent
FILES = (
    "Makefile",
    "README.md",
    "LICENSE",
    "linrad-dvb.example.ini",
    "linrad-dvbt2-eindhoven.example.ini",
    "benchmarks/dvbt2-ldpc-2026-11-05.json",
    "direwolf-pcm.conf",
)
DIRECTORIES = ("src", "components", "third_party", "tools", "tests")
SKIP_DIRS = {"__pycache__", ".git", ".pytest_cache", "build", "dist"}
SKIP_SUFFIXES = {".pyc", ".pyo", ".o", ".a", ".so", ".log"}


def main():
    sources = [ROOT / name for name in FILES]
    for name in DIRECTORIES:
        directory = ROOT / name
        if not directory.is_dir():
            raise FileNotFoundError(directory)
        sources.extend(
            path for path in sorted(directory.rglob("*"))
            if path.is_file()
            and not SKIP_DIRS.intersection(path.relative_to(ROOT).parts)
            and path.suffix not in SKIP_SUFFIXES
        )

    destination = ROOT / "dist" / "linrad_dvb-source.zip"
    destination.parent.mkdir(exist_ok=True)
    # Replace the previous archive only after the new archive is complete.
    with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
        archive = Path(temporary) / destination.name
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
            for path in sources:
                output.write(path, Path("linrad_dvb-source") / path.relative_to(ROOT))
        archive.replace(destination)
    print(f"Created {destination} ({len(sources)} files)")


if __name__ == "__main__":
    main()
