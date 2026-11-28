# USB media handoff for ZuluSCSI RP2040

Notes, tools and firmware for a prototype that lets a PC edit a disk image on
a ZuluSCSI's SD card over USB while the board keeps emulating SCSI drives.
The image is emulated as a removable drive, and the "disk" is in one place at
a time: with the SCSI host, or with the PC.

The firmware change lives on the branch `feature/usb-media-handoff` of this
repository. It is a prototype. It has been tested over USB on one board; it
has **not yet been used with a SCSI host**.

## Where to start

| You want to | Read |
|---|---|
| Add ZuluSCSI support to a disk tool | [04-app-integration.md](04-app-integration.md), then [03-pc-tool-protocol.md](03-pc-tool-protocol.md) |
| Run the test with the sampler | [05-sampler-test.md](05-sampler-test.md) |
| Report what you found | [06-field-reports.md](06-field-reports.md) |
| Understand why it is built this way | [01-findings.md](01-findings.md) |
| Understand or change the firmware | [02-prototype-design.md](02-prototype-design.md) |
| Install the firmware | [firmware/README.md](firmware/README.md) |

## Files

| File | Content |
|---|---|
| `01-findings.md` | Why USB access during emulation does not work in stock firmware, and which designs were considered |
| `02-prototype-design.md` | What the firmware does, its limits, measured speeds, and the log of bench tests |
| `03-pc-tool-protocol.md` | The serial commands and replies, exactly, and how to open the disk |
| `04-app-integration.md` | What a disk tool has to build on top of that protocol |
| `05-sampler-test.md` | Test procedure with a SCSI host attached |
| `06-field-reports.md` | Findings from use outside the bench, added by whoever makes them |
| `firmware/` | The tested firmware build |
| `tools/handoff.py` | Send one command (`status`, `take`, `give`) and print the reply |
| `tools/handoff_console.py` | Keep the serial port open, record the log to a file, send commands |
| `tools/find_device.py` | Find the board's serial port and drive letter on Windows |
| `tools/test_image.py` | Patterned test image, raw read and write checks, checksum of a disk |

The tools need Python 3 with `pyserial`.

## Who maintains what

- Files `01` to `05`, `firmware/` and `tools/` describe the firmware and are
  kept in step with it. If one of them is wrong, say so in
  `06-field-reports.md` and leave the file as it is, so that the correction
  is checked against the firmware source before it is made.
- `06-field-reports.md` is for everyone who uses the prototype.
