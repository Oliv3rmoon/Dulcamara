# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the data-only shelf from an unpacked copy or the Dulcamara source tree."""
import argparse
from pathlib import Path
from tempfile import NamedTemporaryFile
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

FILES = ("LICENSE", "README.md", "game/python-packages/dulcamara_shelf.py", "game/shelf.rpy", "package.py")


def build(destination):
    source = Path(__file__).resolve().parent
    with NamedTemporaryFile(dir=destination.parent, delete=False) as temporary:
        path = Path(temporary.name)
    try:
        with ZipFile(path, "w") as archive:
            for name in FILES:
                entry = ZipInfo("Dulcamara itch.io Shelf/" + name, date_time=(2026, 11, 21, 0, 0, 0))
                entry.create_system = 3
                entry.external_attr = 0o100644 << 16
                archive.writestr(entry, (source / name).read_bytes(), compress_type=ZIP_DEFLATED)
        path.replace(destination)
    finally:
        path.unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    build(parser.parse_args().output)
