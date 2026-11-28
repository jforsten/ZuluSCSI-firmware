#!/usr/bin/env python3
"""Keep the ZuluSCSI serial port open, record its log, and send '@' commands.

For test sessions with a SCSI host: everything the board logs is shown and
written to a file with the time of day, so that it can be matched with what
happened on the host. What you type:

    @status, @take, @give [id]   sent to the board
    any other text               written to the log file as a note, not sent
    quit                         end

    handoff_console.py COM4 --log sampler-test.log

Only lines that start with '@' reach the board, so a typing mistake cannot
hit a key of the firmware's serial menu. While this runs, no other program
(handoff.py, the disk tool) can open the same serial port.

Needs pyserial (pip install pyserial).
"""

import argparse
import datetime
import sys
import threading

import serial


def stamp():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


class Recorder:
    """Prints lines and appends them to the log file, one writer at a time."""

    def __init__(self, path):
        self.lock = threading.Lock()
        self.file = open(path, "a", encoding="utf-8") if path else None

    def write(self, kind, text):
        line = "%s %s %s" % (stamp(), kind, text)
        with self.lock:
            print(line, flush=True)
            if self.file:
                self.file.write(line + "\n")
                self.file.flush()


def read_board(port, recorder, stop):
    buffer = b""
    while not stop.is_set():
        try:
            chunk = port.read(256)
        except serial.SerialException as error:
            recorder.write("!!", "serial port lost: %s" % error)
            stop.set()
            return
        buffer += chunk
        while b"\n" in buffer:
            raw, buffer = buffer.split(b"\n", 1)
            text = raw.decode("ascii", errors="replace").rstrip()
            if text:
                recorder.write("<<", text)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("port", help="serial port of the ZuluSCSI, e.g. COM4")
    parser.add_argument("--log", help="file to append everything to")
    args = parser.parse_args()

    recorder = Recorder(args.log)
    stop = threading.Event()

    # Never 1200 baud: that asks the board to restart into its USB bootloader
    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        reader = threading.Thread(target=read_board, args=(port, recorder, stop), daemon=True)
        reader.start()
        recorder.write("--", "console started on %s" % args.port)

        while not stop.is_set():
            try:
                line = input().strip()
            except (EOFError, KeyboardInterrupt):
                break
            if not line:
                continue
            if line.lower() in ("quit", "exit"):
                break
            if line.startswith("@"):
                if len(line) > 31:
                    recorder.write("!!", "not sent, longer than 31 characters: %s" % line)
                    continue
                recorder.write(">>", line)
                port.write((line + "\r").encode("ascii"))
                port.flush()
            else:
                recorder.write("##", line)

        stop.set()
        reader.join(timeout=1)
        recorder.write("--", "console ended")
    return 0


if __name__ == "__main__":
    sys.exit(main())
