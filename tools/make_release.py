#!/usr/bin/env python3
"""Package a finished `pio run -e t3s3` build into release files.

Writes into dist/ (or the directory given with --out):

  lilygo-power-<ver>-update.bin    the app image alone. This is what the web UI
                                   takes under System -> Firmware-Update (OTA);
                                   settings are kept.
  lilygo-power-<ver>-factory.bin   bootloader + partition table + boot pointer +
                                   app merged into one file, written at 0x0 with
                                   esptool. A complete fresh install: the gap
                                   between the partition table and the boot
                                   pointer is the settings (NVS) partition, and
                                   merging fills it with 0xFF, so every setting
                                   is erased.
  parts/                           the same four pieces separately, plus
  manifest.json                    an ESP Web Tools manifest that writes them at
                                   their own offsets - flashing from the browser
                                   therefore keeps the settings unless "erase" is
                                   ticked.
  SHA256SUMS                       checksums of everything above.

Offsets come from the partition table, never from constants, for the same
reason flash.sh reads them: a layout change must not silently produce an image
linked for one address and written to another.

Usage: python3 tools/make_release.py [--version 1.2.3] [--out dist]
"""

from __future__ import annotations

import argparse
import csv
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE = os.path.join(ROOT, "firmware")
BUILD = os.path.join(FIRMWARE, ".pio", "build", "t3s3")
PART_CSV = os.path.join(FIRMWARE, "partitions", "lilygo_4mb_ota.csv")
CHIP = "esp32s3"
BOOTLOADER_OFFSET = 0x0  # ESP32-S3 ROM loads the second stage from 0x0
PARTITION_TABLE_OFFSET = 0x8000


def partition_offsets() -> dict[str, int]:
    offsets: dict[str, int] = {}
    with open(PART_CSV, encoding="utf-8") as handle:
        rows = (line for line in handle if line.strip() and not line.lstrip().startswith("#"))
        for row in csv.reader(rows):
            cells = [c.strip() for c in row]
            if len(cells) >= 4 and cells[3]:
                offsets[cells[0]] = int(cells[3], 0)
    for needed in ("otadata", "app0"):
        if needed not in offsets:
            sys.exit(f"partition table has no '{needed}' entry: {PART_CSV}")
    return offsets


def find_boot_app0() -> str:
    """The 8 KB otadata image that points the bootloader at app0."""
    candidates = glob.glob(
        os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin")
    )
    if not candidates:
        sys.exit("boot_app0.bin not found - run 'pio run -e t3s3' first so the framework is installed")
    return sorted(candidates)[0]


def read_version(explicit: str | None) -> str:
    if explicit:
        return explicit.lstrip("v")
    with open(os.path.join(ROOT, "VERSION"), encoding="utf-8") as handle:
        return handle.read().strip() or "0.0.0"


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", help="version string (default: the VERSION file)")
    parser.add_argument("--out", default=os.path.join(ROOT, "dist"))
    args = parser.parse_args()

    version = read_version(args.version)
    offsets = partition_offsets()
    sources = {
        "bootloader.bin": (BOOTLOADER_OFFSET, os.path.join(BUILD, "bootloader.bin")),
        "partitions.bin": (PARTITION_TABLE_OFFSET, os.path.join(BUILD, "partitions.bin")),
        "boot_app0.bin": (offsets["otadata"], find_boot_app0()),
        "firmware.bin": (offsets["app0"], os.path.join(BUILD, "firmware.bin")),
    }
    for name, (_, path) in sources.items():
        if not os.path.isfile(path):
            sys.exit(f"missing build output {path} - run 'pio run -e t3s3' first")

    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    parts_dir = os.path.join(args.out, "parts")
    os.makedirs(parts_dir)

    base = f"lilygo-power-{version}"
    update_bin = os.path.join(args.out, f"{base}-update.bin")
    factory_bin = os.path.join(args.out, f"{base}-factory.bin")
    shutil.copyfile(sources["firmware.bin"][1], update_bin)

    merge_args: list[str] = []
    for name, (offset, path) in sources.items():
        shutil.copyfile(path, os.path.join(parts_dir, name))
        merge_args += [hex(offset), path]
    subprocess.run(
        [sys.executable, "-m", "esptool", "--chip", CHIP, "merge-bin", "-o", factory_bin, *merge_args],
        check=True,
    )

    # ESP Web Tools: separate parts at their offsets, so NVS is left alone.
    manifest = {
        "name": "LilyGO T3-S3 Power Meter",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "parts": [{"path": f"parts/{name}", "offset": offset} for name, (offset, _) in sources.items()],
            }
        ],
    }
    with open(os.path.join(args.out, "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2)

    sums = []
    for path in sorted(glob.glob(os.path.join(args.out, "**", "*"), recursive=True)):
        if os.path.isfile(path):
            sums.append(f"{sha256(path)}  {os.path.relpath(path, args.out)}")
    with open(os.path.join(args.out, "SHA256SUMS"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(sums) + "\n")

    for name, (offset, _) in sources.items():
        print(f"  {hex(offset):>9}  {name}")
    print(f"release {version} written to {args.out}")


if __name__ == "__main__":
    main()
