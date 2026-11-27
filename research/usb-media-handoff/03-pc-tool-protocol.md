# USB media handoff: protocol for a PC tool

What a PC program has to do, in order, to read and write an image through a
ZuluSCSI running the handoff prototype. Written for whoever adds this to a
disk tool (for example an EpsLin-based one). The firmware side is described in
[02-prototype-design.md](02-prototype-design.md).

Everything here was either read from the firmware source or observed in the
bench tests on Windows 11 (2026-11-27). Items that were not tested are marked.

## Overview

The board is one USB device with two interfaces on the same cable:

- a **serial port**, used for control (`@take`, `@give`, `@status`),
- a **removable disk** per participating image, used for the data.

The disk has no media until the tool asks for it over the serial port. While
the tool has the media, the SCSI host sees an ejected drive.

```
serial:  @take ──► "@OK take"          ...            @give ──► "@OK give"
disk:    (no media)  ─► media appears ─► read/write ─► flush, close ─► (no media)
SCSI:    has media   ─► drive ejected ───────────────────────────────► media back
```

## Finding the device

| What | Value |
|---|---|
| USB vendor / product ID | `2E8A` / `10D7` |
| Product string | `ZuluSCSI RP2040` |
| Serial interface | interface 0, name `Pico Serial` (Windows: "USB Serial Device (COMn)") |
| Disk | vendor `ZuluSCSI`, product `RP2040` (Windows: "ZuluSCSI RP2040 USB Device") |

Check the product ID or product string before sending anything. Other
Raspberry Pi based devices share the vendor ID.

Stock firmware older than the handoff prototype may report a different ID
(the 2023 release showed `2E8A:00C0`, "RaspberryPi Pico") and does not
understand the commands below.

## Serial port

- Any baud rate **except 1200**. According to the USB core's source, 1200
  baud with DTR low reboots the board into its USB bootloader (not tried on
  the prototype firmware). The tests used 115200.
- **DTR must be asserted.** The firmware sends nothing while DTR is low. Most
  serial libraries assert it when the port is opened.
- The port also carries the firmware log. Replies are ordinary log lines, so
  they have a time prefix and can be mixed with unrelated lines:

  ```
  [36101ms] @OK status luns=1
  ```

  To parse: split the stream into lines, find the first `@` in each line, and
  ignore lines without one.
- Before sending a command, discard input that is already waiting.

### Sending a command

- A command is one line that starts with `@` and ends with CR or LF.
- At most 31 characters. Command words are not case sensitive.
- Send the whole line in one go. A line that stops arriving for 2 seconds is
  dropped.
- A newer `@take` or `@give` for the same drive replaces one that has not
  been carried out yet.

### Commands and replies

`id` is the SCSI ID of the drive. It can be left out when the board has
exactly one participating drive.

| Command | Reply on success | Meaning |
|---|---|---|
| `@status` | one `@LUN ...` line per drive, then `@OK status luns=<n>` | List drives and who has the media |
| `@take [id]` | `@OK take id=<id> sectors=<n> ro=<0\|1>` | Media goes to USB |
| `@give [id]` | `@OK give id=<id>` | Media goes back to the SCSI host |

`@LUN` lines:

```
@LUN lun=0 id=0 owner=scsi sectors=196608 ro=0
@LUN lun=0 id=0 owner=usb sectors=196608 ro=0
@LUN lun=1 id=4 owner=scsi unavailable=not-contiguous
```

- `lun` is the USB LUN, counted in SCSI ID order. Each LUN is a separate disk
  on the PC.
- `sectors` is the size in 512 byte sectors.
- `ro=1` means the image is read-only; writes over USB will fail.
- `unavailable=<reason>` means `@take` would fail for that reason.

Both commands can be repeated safely: `@take` when USB already has the media
and `@give` when the SCSI host already has it both answer `@OK`.

`@OK give id=<id> forced=1` means USB was still reading or writing after 5
seconds and the firmware went ahead anyway. Treat it as a warning that data
may not have been complete.

### Errors

```
@ERR take id=0 reason=not-contiguous
@ERR take reason=bad-id
@ERR unknown-command, use @status, @take [id] or @give [id]
```

| Reason | Meaning |
|---|---|
| `disabled` | `USBMediaHandoff` is not set, or no drive takes part |
| `bad-id` | The ID is missing (and there is more than one drive) or not a number |
| `no-such-lun` | That SCSI ID is not a participating drive |
| `no-card` | No SD card |
| `no-image` | No image is open for that ID, or it is not a removable-media type |
| `sector-size` | The image does not use 512 byte sectors |
| `not-contiguous` | The image file is fragmented on the SD card (or is a ROM, copy-on-write or gapped image) |

### Unsolicited lines

These can arrive at any time and are not replies to a command:

| Line | Meaning |
|---|---|
| `@EVENT give id=<id> source=usb-eject` | The PC ejected the disk; followed by `@OK give id=<id>` |
| `@EVENT take id=<id> source=usb-load` | The PC sent a load command; followed by `@OK take ...` |
| `@EVENT revoke id=<id> reason=<r>` | The firmware took the media away from USB. Stop using the disk. Reasons: `image-changed` (another image was loaded for that ID), `images-closed`, `no-card` (SD card removed), `image-state` (the image is no longer in the state it was handed over in). Not triggered in the tests. |
| `@EVENT io-error id=<id> count=<n>` | An SD card read or write for USB failed; `n` is the running total |

### Timing

- Commands are carried out between SCSI commands. With an idle SCSI bus the
  reply came within a second in the tests. During a long SCSI transfer on
  another drive it can take as long as that transfer. Use a timeout of 15
  seconds or more.
- `@give` waits until USB has done no read or write for 300 ms, at most 5 s.
- `@status` answered in about 0.3 s while a USB read was running.

## The disk

### States

| Media owner | What the PC sees |
|---|---|
| SCSI | Removable disk without media. Opening it or reading fails ("not ready"). |
| USB | Disk with media, 512 byte sectors, `sectors` from the `@OK take` reply. |

After `@OK take` the operating system needs a moment to notice the media. In
the tests on Windows it was there within 3 seconds. Poll until the disk
reports a size, or retry opening it, rather than using a fixed delay.

### Opening it

- **Windows**: `\\.\F:` (the disk's drive letter) can be opened for raw
  reading and writing **without administrator rights**, as long as Windows
  does not recognise a file system on it. `\\.\PhysicalDriveN` needs
  administrator rights.
  - With no partition table on the disk, sector N of `\\.\F:` is sector N of
    the image. This was verified. If sector 0 ever looks like a partition
    table, the drive letter would start at the first partition instead; that
    case was not tested.
  - Explorer may offer to format the disk because it sees no file system.
    Decline.
- **Linux / macOS** (not tested): the whole-disk device, e.g. `/dev/sdX`.

### Reading and writing

- Whole 512 byte sectors only, at sector-aligned offsets, inside the size
  given by `@OK take`. Open the device unbuffered.
- Measured speed: about 650 kB/s reading, about 540 kB/s writing (USB full
  speed). Write only the sectors that changed.
- Before giving the media back: flush (`FlushFileBuffers` / `fsync`) and
  **close the device**. Nothing the operating system still holds reaches the
  card after `@give`.

### Eject from the PC

Ejecting the disk in the operating system gives the media back, the same as
`@give`. "Eject" in Windows Explorer does this (tested). Other ways of
removing a device, and other operating systems, were not tested. A tool
should use `@give` and not rely on eject.

## Complete sequence

1. Find the serial port and the disk by USB ID. Open the serial port (not at
   1200 baud).
2. Send `@status`. Check that the drive is listed and not `unavailable`.
3. Send `@take`. Wait for `@OK take` and note `sectors` and `ro`. On `@ERR`,
   stop and report the reason.
4. Wait until the disk reports media, then open it raw.
5. Read and write sectors.
6. Flush and close the disk.
7. Send `@give`. Wait for `@OK give`. If the reply has `forced=1`, warn.
8. Close the serial port.

If `@EVENT revoke` arrives during steps 4 to 6, or a read or write fails,
close the disk and go back to step 2.

Keep the time between steps 3 and 7 short. The SCSI host has no disk during
that time.

## What the SCSI host sees

- During the handoff: the drive answers but reports NOT READY, medium not
  present.
- After `@give`: with `EnableUnitAttention = 1` the next command gets UNIT
  ATTENTION, "medium may have changed", once. The host is expected to
  re-read the disk. Whether an Ensoniq sampler does that by itself has not
  been tested.
- `@take` does not ask the host. If the host is in the middle of loading or
  saving, it is the same as pulling the disk out. Take the media only while
  the host is idle.

## Reference implementation

- [tools/handoff.py](tools/handoff.py): sends one command and waits for the
  reply, with the line parsing described above.
- [tools/test_image.py](tools/test_image.py): raw sector reads and writes on
  the disk (`verify`, `mark`, `unmark`), used for the bench tests.

Example session on Windows, as run in the tests:

```
handoff.py COM4 status          @LUN lun=0 id=0 owner=scsi sectors=196608 ro=0
                                @OK status luns=1
handoff.py COM4 take            @OK take id=0 sectors=196608 ro=0
test_image.py verify \\.\F:     checked 196608 sectors, 0 wrong
handoff.py COM4 give            @OK give id=0
```
