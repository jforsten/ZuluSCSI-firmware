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

Write speed, without changing the image (every sector is rewritten with the
pattern it already holds):

    test_image.py rewrite \\.\F: --sectors 16384        # 8 MB from sector 0
    test_image.py rewrite \\.\F: --start 50000 --sectors 4096

Checksum of any disk or image, read-only, for before/after comparisons. Works
on a real (for example Ensoniq) image too:

    test_image.py hash \.\F: --sectors 196608       # sector count from "@OK take"

This is not an Ensoniq disk. It only exists to test the handoff. "mark",
"unmark" and "rewrite" write the test pattern and destroy any other content;
never run them on a real image.
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


def rewrite(path, start, sectors):
    """Write the original pattern over a run of sectors and report the speed."""
    import time
    sectors = min(sectors, ZIP100_SECTORS - start)
    begin = time.monotonic()
    with open(path, "r+b", buffering=0) as dev:
        dev.seek(start * SECTOR_SIZE)
        for first in range(start, start + sectors, CHUNK_SECTORS):
            n = min(CHUNK_SECTORS, start + sectors - first)
            data = make_chunk(first, n)
            if dev.write(data) != len(data):
                print("short write at sector %d" % first)
                return 1
    elapsed = time.monotonic() - begin
    print("rewrote %d sectors from %d in %.2f s = %.0f kB/s"
          % (sectors, start, elapsed, sectors * SECTOR_SIZE / 1024 / elapsed))
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


def sha256(path, sectors):
    """Print the SHA-256 of the first 'sectors' sectors. Reads only."""
    import hashlib
    import time
    digest = hashlib.sha256()
    begin = time.monotonic()
    with open(path, "rb", buffering=0) as src:
        for first in range(0, sectors, CHUNK_SECTORS):
            count = min(CHUNK_SECTORS, sectors - first)
            data = src.read(count * SECTOR_SIZE)
            if len(data) != count * SECTOR_SIZE:
                print("short read at sector %d: got %d bytes" % (first, len(data)))
                return 1
            digest.update(data)
    print("sha256 of %d sectors: %s (%.0f s)" % (sectors, digest.hexdigest(), time.monotonic() - begin))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["create", "verify", "hash", "mark", "unmark", "rewrite"])
    parser.add_argument("path")
    parser.add_argument("--sectors", type=int, default=ZIP100_SECTORS,
                        help="verify, hash or rewrite only this many sectors")
    parser.add_argument("--start", type=int, default=0,
                        help="rewrite: first sector (default 0)")
    parser.add_argument("--marked", action="store_true",
                        help="verify: expect the marker data written by 'mark'")
    args = parser.parse_args()

    if args.command == "create":
        return create(args.path)
    if args.command == "mark":
        return write_runs(args.path, True)
    if args.command == "unmark":
        return write_runs(args.path, False)
    if args.command == "hash":
        return sha256(args.path, args.sectors)
    if args.command == "rewrite":
        return rewrite(args.path, args.start, args.sectors)
    return verify(args.path, args.sectors, args.marked)


if __name__ == "__main__":
    sys.exit(main())
