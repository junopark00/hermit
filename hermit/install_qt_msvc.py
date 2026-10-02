"""Install the prebuilt Qt 6 MSVC x64 SDK that Hermit needs, straight from download.qt.io.

aqtinstall 3.3.0 does not understand the per-architecture repository layout Qt uses for
6.11 (qt6_6113/qt6_6113_msvc2022_64/Updates.xml), so this reads Updates.xml itself,
downloads only the archives of the main desktop package (skipping docs), verifies each
archive against the .sha1 published next to it, and extracts it with py7zr (installed as
an aqtinstall dependency, or `pip install py7zr`).

Usage:
    python hermit/install_qt_msvc.py [--version 6.11.3] [--dest C:/Qt]

Result: <dest>/<version>/msvc2022_64 with bin/qmake.exe. Nothing outside <dest> is changed.
"""
import argparse
import hashlib
import os
import sys
import tempfile
import urllib.request
import xml.etree.ElementTree as ET

import py7zr

BASE = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop"
ARCH = "msvc2022_64"
SKIP_PREFIXES = ("qtdoc-",)


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=120) as response:
        return response.read()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", default="6.11.3")
    parser.add_argument("--dest", default="C:/Qt")
    args = parser.parse_args()

    compact = args.version.replace(".", "")
    repo = f"{BASE}/qt6_{compact}/qt6_{compact}_{ARCH}"
    package = f"qt.qt6.{compact}.win64_{ARCH}"
    target = os.path.join(args.dest, args.version, ARCH)
    if os.path.exists(os.path.join(target, "bin", "qmake.exe")):
        print(f"Qt already installed at {target}")
        return 0

    root = ET.fromstring(fetch(f"{repo}/Updates.xml"))
    update = next((p for p in root.findall("PackageUpdate") if p.findtext("Name") == package), None)
    if update is None:
        print(f"Package {package} not found in {repo}/Updates.xml", file=sys.stderr)
        return 1
    version = update.findtext("Version")
    archives = [a.strip() for a in (update.findtext("DownloadableArchives") or "").split(",") if a.strip()]
    archives = [a for a in archives if not a.startswith(SKIP_PREFIXES)]
    print(f"{package} {version}: {len(archives)} archives")

    os.makedirs(args.dest, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for name in archives:
            url = f"{repo}/{package}/{version}{name}"
            expected = fetch(url + ".sha1").decode("ascii").split()[0].strip().lower()
            data = fetch(url)
            actual = hashlib.sha1(data).hexdigest()
            if actual != expected:
                print(f"Checksum mismatch for {name}: expected {expected}, got {actual}", file=sys.stderr)
                return 1
            path = os.path.join(tmp, name)
            with open(path, "wb") as f:
                f.write(data)
            print(f"  verified {name} ({len(data) // (1024 * 1024)} MB)")

        for name in archives:
            with py7zr.SevenZipFile(os.path.join(tmp, name), "r") as archive:
                # Archives either carry the <version>/<arch>/ prefix or are relative to the arch dir.
                names = archive.getnames()
                prefixed = any(n.replace("\\", "/").startswith(f"{args.version}/{ARCH}") for n in names)
                archive.extractall(args.dest if prefixed else target)
            print(f"  extracted {name}")

    if not os.path.exists(os.path.join(target, "bin", "qmake.exe")):
        print(f"qmake.exe not found under {target} after extraction", file=sys.stderr)
        return 1
    print(f"Qt {args.version} installed at {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
