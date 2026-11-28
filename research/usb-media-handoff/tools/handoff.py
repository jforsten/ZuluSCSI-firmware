#!/usr/bin/env python3
"""Control the ZuluSCSI USB media handoff from a PC.

Sends the '@' commands described in ../02-prototype-design.md over the USB
serial port and waits for the reply.

    handoff.py COM7 status
    handoff.py COM7 take          # media to USB (SCSI side sees an eject)
    handoff.py COM7 give          # media back to the SCSI host
    handoff.py /dev/ttyACM0 take 5

Needs pyserial (pip install pyserial).
"""

import argparse
import re
import sys
import time

import serial

# A log line whose message starts with '@': time prefix, then the reply
REPLY_LINE = re.compile(r"^\[\d+ms\] (@.*)$")


def read_reply(port, command, timeout):
    """Collect '@' lines until the '@OK <command>' or '@ERR' line arrives."""
    deadline = time.monotonic() + timeout
    lines = []
    buffer = b""
    while time.monotonic() < deadline:
        chunk = port.read(256)
        if not chunk:
            continue
        buffer += chunk
        while b"\n" in buffer:
            raw, buffer = buffer.split(b"\n", 1)
            text = raw.decode("ascii", errors="replace").strip()

            # Replies come through the firmware log: "[1234ms] @OK take id=5".
            # Other log lines can contain an '@' further in, so only lines
            # whose message starts with it count.
            match = REPLY_LINE.match(text)
            if not match:
                continue
            reply = match.group(1)
            lines.append(reply)
            if reply.startswith("@OK " + command) or reply.startswith("@ERR"):
                return lines
    raise TimeoutError("no reply to @%s within %.0f s; received: %r" % (command, timeout, lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("port", help="serial port of the ZuluSCSI, e.g. COM7 or /dev/ttyACM0")
    parser.add_argument("command", choices=["status", "take", "give"])
    parser.add_argument("scsi_id", nargs="?", type=int,
                        help="SCSI ID of the drive; may be left out if there is only one")
    parser.add_argument("--timeout", type=float, default=15.0,
                        help="seconds to wait for the reply (default 15)")
    args = parser.parse_args()

    line = "@" + args.command
    if args.scsi_id is not None:
        if args.command == "status":
            parser.error("status takes no SCSI ID")
        line += " %d" % args.scsi_id

    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        port.reset_input_buffer()
        port.write((line + "\r").encode("ascii"))
        port.flush()
        try:
            replies = read_reply(port, args.command, args.timeout)
        except TimeoutError as error:
            print(error, file=sys.stderr)
            return 2

    for reply in replies:
        print(reply)
    return 0 if replies[-1].startswith("@OK") else 1


if __name__ == "__main__":
    sys.exit(main())
