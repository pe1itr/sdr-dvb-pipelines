#!/usr/bin/env python3
"""Package the current Linrad DVB sources without requiring Git or a build."""

from pathlib import Path
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parent.parent
FILES = (
    ".gitignore",
    "Makefile",
    "README.md",
    "LICENSE",
    "NOTICE",
    "linrad-dvb.example.ini",
    "linrad-dvbt2-eindhoven.example.ini",
    "direwolf-pcm.conf",
)
DIRECTORIES = ("src", "components", "third_party", "tools", "dev", "tests", "docs", "benchmarks")
SKIP_DIRS = {"__pycache__", ".git", ".pytest_cache", "build", "dist", "target"}
SKIP_SUFFIXES = {".pyc", ".pyo", ".o", ".a", ".so", ".d", ".log", ".iq", ".ts", ".zip"}
SCREENSHOT = Path("docs/images/linrad-dvb-dvbt2.png")


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
            and not path.name.endswith(".iq.json")
            and (path.suffix.lower() not in {".png", ".jpg", ".jpeg"}
                 or path.relative_to(ROOT) == SCREENSHOT)
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
