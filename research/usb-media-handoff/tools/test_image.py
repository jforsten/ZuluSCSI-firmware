#!/usr/bin/env python3
r"""Create or check a Zip 100 test image with a known pattern in every sector.

Each 512 byte sector holds its own sector number, so a read over USB can be
checked without a reference copy, and a sector that comes from the wrong place
on the SD card is detected.

    test_image.py create K:\ZP0_handoff_test.img
    test_image.py verify K:\ZP0_handoff_test.img       # the file on the card
    test_image.py verify \\.\F:                        # the USB drive, by drive letter
    test_image.py verify \\.\PhysicalDrive3            # the same, needs admin on Windows
    test_image.py verify /dev/sdX --sectors 2048       # only the first 2048 sectors

Write test, on the USB drive while USB owns the media:

    test_image.py mark \\.\F:              # overwrite the test sectors with marker data
    test_image.py verify \\.\F: --marked   # whole image, expecting the markers
    test_image.py unmark \\.\F:            # put the original pattern back

This is not an Ensoniq disk. It only exists to test the handoff.
"""

import argparse
import struct
import sys

SECTOR_SIZE = 512
ZIP100_SECTORS = 100663296 // SECTOR_SIZE  # 196608, what ZuluSCSI expects for Zip 100
MAGIC = b"ZULUTEST"
CHUNK_SECTORS = 128  # 64 kB per read or write
MARK_MAGIC = b"WRITETST"

# Sector runs (first sector, count) that the write test overwrites: both ends
# of the image, a short run, a run across a 64k sector boundary and 1 MB.
MARK_RUNS = [
    (0, 1),
    (1000, 16),
    (65535, 2),
    (100000, 2048),
    (ZIP100_SECTORS - 1, 1),
]


def make_sector(lba):
    filler = struct.pack("<I", lba ^ 0xA5A5A5A5) * ((SECTOR_SIZE - 12) // 4)
    return struct.pack("<I", lba) + MAGIC + filler


def make_marked_sector(lba):
    filler = struct.pack("<I", lba ^ 0x5A5A5A5A) * ((SECTOR_SIZE - 12) // 4)
    return struct.pack("<I", lba) + MARK_MAGIC + filler


def is_marked(lba):
    return any(first <= lba < first + count for first, count in MARK_RUNS)


def make_chunk(first_lba, count, marked=False):
    return b"".join(make_marked_sector(lba) if marked and is_marked(lba) else make_sector(lba)
                    for lba in range(first_lba, first_lba + count))


def write_runs(path, marked):
    """Overwrite the test sector runs, with marker data or the original pattern."""
    import time
    total = 0
    start = time.monotonic()
    # Unbuffered: raw devices only accept whole-sector writes
    with open(path, "r+b", buffering=0) as dev:
        for first, count in MARK_RUNS:
            dev.seek(first * SECTOR_SIZE)
            for offset in range(0, count, CHUNK_SECTORS):
                n = min(CHUNK_SECTORS, count - offset)
                data = make_chunk(first + offset, n, marked)
                written = dev.write(data)
                if written != len(data):
                    print("short write at sector %d: %d bytes" % (first + offset, written))
                    return 1
                total += n
    elapsed = time.monotonic() - start
    print("wrote %d sectors with %s in %.1f s (%.0f kB/s)"
          % (total, "marker data" if marked else "the original pattern", elapsed,
             total * SECTOR_SIZE / 1024 / elapsed))
    return 0


def create(path):
    with open(path, "wb") as out:
        for first in range(0, ZIP100_SECTORS, CHUNK_SECTORS):
            out.write(make_chunk(first, min(CHUNK_SECTORS, ZIP100_SECTORS - first)))
    print("wrote %d sectors (%d bytes) to %s" % (ZIP100_SECTORS, ZIP100_SECTORS * SECTOR_SIZE, path))
    return 0


def verify(path, sectors, marked=False):
    bad = 0
    checked = 0
    # Unbuffered: raw devices only accept whole-sector reads
    with open(path, "rb", buffering=0) as src:
        for first in range(0, sectors, CHUNK_SECTORS):
            count = min(CHUNK_SECTORS, sectors - first)
            data = src.read(count * SECTOR_SIZE)
            if len(data) != count * SECTOR_SIZE:
                print("short read at sector %d: got %d bytes" % (first, len(data)))
                return 1
            if data != make_chunk(first, count, marked):
                for i in range(count):
                    got = data[i * SECTOR_SIZE:(i + 1) * SECTOR_SIZE]
                    if got != make_chunk(first + i, 1, marked):
                        bad += 1
                        if bad <= 5:
                            found = struct.unpack("<I", got[:4])[0]
                            print("sector %d is wrong (starts with %s, sector field %d)"
                                  % (first + i, got[4:12], found))
            checked += count
    print("checked %d sectors%s, %d wrong"
          % (checked, " (expecting markers)" if marked else "", bad))
    return 1 if bad else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["create", "verify", "mark", "unmark"])
    parser.add_argument("path")
    parser.add_argument("--sectors", type=int, default=ZIP100_SECTORS,
                        help="verify only this many sectors from the start")
    parser.add_argument("--marked", action="store_true",
                        help="verify: expect the marker data written by 'mark'")
    args = parser.parse_args()

    if args.command == "create":
        return create(args.path)
    if args.command == "mark":
        return write_runs(args.path, True)
    if args.command == "unmark":
        return write_runs(args.path, False)
    return verify(args.path, args.sectors, args.marked)


if __name__ == "__main__":
    sys.exit(main())
