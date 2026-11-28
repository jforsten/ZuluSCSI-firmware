# Adding ZuluSCSI RP2040 support to a disk tool

For whoever adds a ZuluSCSI backend to a tool that reads and writes disk
images sector by sector (the case in mind is an EpsLin-derived Ensoniq tool
that already supports SCSI2SD V6 and PiSCSI). It assumes no knowledge of the
ZuluSCSI firmware.

This file says what to build and what to watch out for. The exact commands,
replies and timings are in [03-pc-tool-protocol.md](03-pc-tool-protocol.md);
read that next. Findings go into [06-field-reports.md](06-field-reports.md).

## Status of what you are building on

- A **prototype** firmware for ZuluSCSI RP2040 boards, on the branch
  `feature/usb-media-handoff`. It is not part of any official release.
- Tested over USB on one ZuluSCSI Mini RP2040 with Windows 11: reads, writes,
  take, give, eject. **Never yet used with a SCSI host attached.**
- Each statement below is marked when it comes from reading the source or is
  a guess, and not from a test.

## The model

The image is a file on the SD card inside the ZuluSCSI. The board presents it
to the SCSI host as a removable drive (Iomega Zip 100 in the planned setup).
Over the one USB cable the PC gets two things: a serial port and a removable
USB disk.

The media is in one place at a time:

| Media is with | SCSI host sees | PC sees |
|---|---|---|
| SCSI host (the state after power-on) | drive with a disk in it | USB disk without media |
| PC | drive without a disk | USB disk with media: the image, sector for sector |

The PC moves the media with two serial commands: `@take` brings it to the
PC, `@give` returns it. To each side this looks like a disk being removed and
inserted. There is no moment at which both sides can reach the image.

On the USB disk, sector N is sector N of the image file, which is sector N
of the drive the SCSI host sees. Sectors are 512 bytes. There is no partition
table or other wrapping.

### Compared with the two backends the tool has

This is how the three look from here. The tool's own code was not seen when
this was written, so correct the last column where it is wrong.

| | SCSI2SD V6 | PiSCSI | ZuluSCSI handoff |
|---|---|---|---|
| Where the image is | raw sector range on the SD card | file on the Pi | file on the SD card |
| How the PC reaches it | USB disk, raw sectors | over the network | USB disk, raw sectors |
| Access while the SCSI host has the disk | possible, nothing prevents it | image is detached first (assumed) | **not possible**, the USB disk has no media |
| Step before and after access | none | detach / attach (assumed) | `@take` / `@give` on the serial port |
| SCSI host is told the disk changed | no | on attach (assumed) | yes, as a media change |

In practice: the sector access should be the code path used for SCSI2SD V6
(a raw USB disk), and the take and give steps sit where detach and attach sit
for PiSCSI. That the SCSI2SD V6 path works unchanged on this disk is an
expectation, not a test result.

## What has to be in place on the board

The tool cannot arrange these; it can only check them with `@status` and
tell the user what is missing.

- Firmware: the build in [firmware/](firmware/README.md) or newer from the
  branch. At start-up it logs
  `FW Version: 2026.11.24-release 2026-11-28T02:12:46 git:48fce31-dirty`.
  Builds older than that one do not stop the SCSI host from reading and
  writing during a handoff and must not be used with a host.
- `zuluscsi.ini` on the SD card:

  ```ini
  [SCSI]
  USBMediaHandoff = 1
  EnableUnitAttention = 1
  ```

- The image:
  - a removable type, chosen by the first two letters of the file name:
    `ZP` Zip 100, `RE` removable disk, `MO` magneto-optical, `FD` floppy.
    The third character is the SCSI ID, e.g. `ZP0_ensoniq.img` is a Zip
    drive on ID 0;
  - a `ZP` image should be exactly 100,663,296 bytes (196,608 sectors). The
    firmware accepts other sizes but logs that the drive "may not work
    correctly";
  - 512 byte sectors;
  - stored on the card as one contiguous block. A file copied onto a freshly
    formatted card is contiguous. A fragmented file is reported as
    `unavailable=not-contiguous`.
- The set of drives is fixed when the board starts. A new or renamed image
  needs a power cycle.

## Finding the board

One board is one USB device with a serial interface and a disk. Tested on
Windows 11 with [tools/find_device.py](tools/find_device.py), which needs no
administrator rights and sends nothing to the board:

1. **Serial port**: the port whose USB IDs are vendor `2E8A`, product `10D7`.
   Note its USB serial number (16 hexadecimal digits, e.g.
   `E66234B51F225629`).
2. **Disk**: among drive letters of type "removable", open `\\.\X:` with no
   access rights and send `IOCTL_STORAGE_QUERY_PROPERTY`
   (`StorageDeviceProperty`). The ZuluSCSI disk answers vendor `ZuluSCSI`,
   product `RP2040`, and its serial number field holds the same USB serial
   number as the port. That ties a drive letter to a serial port when several
   boards are connected. This works while the disk has no media.
3. `IOCTL_STORAGE_GET_DEVICE_NUMBER` on the same handle gives N of
   `\\.\PhysicalDriveN`, if the tool prefers that path. It needs
   administrator rights; the drive letter does not.

Seen in the tests: serial port `COM4` ("USB Serial Device"), disk
"ZuluSCSI RP2040 USB Device", drive letter `F:`. None of these are fixed.

Not tested:

- More than one participating image. Each becomes its own USB disk (LUN) with
  its own drive letter. `find_device.py` lists the letters but does not know
  which LUN is which; after `@take`, compare the disk size with the `sectors`
  value of the reply, or take one drive at a time and see which letter gets
  media.
- Linux and macOS. The serial port should appear as `/dev/ttyACM*` or
  `/dev/cu.usbmodem*` and the disk as a whole-disk device; match them through
  the USB serial number in `/sys` or `ioreg`.

### Is it the right firmware?

The USB IDs are the same for official firmware of the same generation, so
they do not show whether the handoff exists. Ask:

| Reply to `@status` | Meaning |
|---|---|
| `@LUN ...` lines and `@OK status luns=<n>` | Handoff firmware, ready |
| `@ERR status reason=disabled` | Handoff firmware, but the setting is off or no image qualifies |
| No line starting with `@` within the timeout | Not the handoff firmware |

Firmware without the handoff treats every character as a key of its
interactive serial menu. Read from the source, not tested: none of the three
commands contains the confirmation key `y`, so nothing is carried out, and
the firmware answers with its menu text. Two rules follow:

- End each command with **CR** (`\r`). The menu ignores LF, so `@status`
  followed by LF alone would leave the final `s` ("restart as a card
  reader") waiting for a confirmation.
- Never send anything else to this port. In particular never `y`, and never
  open the port at 1200 baud (that asks the board to restart into its USB
  bootloader).

## What the backend does

The complete sequence with every reply is in
[03-pc-tool-protocol.md](03-pc-tool-protocol.md), "Complete sequence". In
terms of a backend with open, read, write and close:

**Open**

1. Find the port and the drive letter.
2. Open the serial port at 115200 baud with DTR asserted. Discard waiting
   input. Keep it open until close.
3. `@status`. Find the drive. If it says `unavailable=<reason>` or the reply
   is an error, stop and tell the user (see "What to tell the user").
4. If it says `owner=usb`, an earlier session did not finish and the media
   is still with the PC. Step 5 is safe to repeat in that state.
5. `@take`. Keep `sectors` and `ro` from the reply.
6. Wait for the media: poll `IOCTL_DISK_GET_LENGTH_INFO` on the drive letter
   until it reports `sectors` × 512 bytes. In the tests this took under 3
   seconds. Without media the volume can still be opened, and the length is
   0; so check the length, not whether the open succeeds.
7. Open the disk for raw access.

**Read and write**: whole sectors, see the next section.

**Close**

8. Flush and close the disk handle.
9. `@give`. Wait for `@OK give`.
10. Close the serial port.

Lines that start with `@EVENT` can arrive at any time while the port is
open. Read the port during the whole session, or at least before each write
and before `@give`, and act on `@EVENT revoke` (see "When things go wrong").

Reply parsing in one sentence: split into lines, drop the `[<number>ms] `
prefix, ignore every line that does not then start with `@`.
[tools/handoff.py](tools/handoff.py) is a 60-line reference.

## Raw access on Windows

What the tests did, all without administrator rights:

- Opened `\\.\F:` for reading and writing with ordinary file calls and no
  buffering (Python `open(path, "r+b", buffering=0)`).
- Sought to sector-aligned offsets, read and wrote in 64 kB pieces. Sizes and
  offsets must be multiples of 512.
- Read the whole 96 MB and wrote several megabytes this way, with every
  sector verified afterwards.

Things to know:

- This works because Windows finds no file system it recognises on the disk.
  The test image held a number pattern, not an Ensoniq file system. An
  Ensoniq disk is expected to behave the same, since Windows does not know
  that format. **Not tested with a real Ensoniq image; please report.**
- If sector 0 of an image ever looked like a PC partition table, the drive
  letter would map to the first partition instead of the whole disk. Not
  tested. `\\.\PhysicalDriveN` always means the whole disk.
- Windows may pop up "You need to format the disk in drive F:". It must be
  declined. The tool cannot suppress it; warn the user once.
- No volume lock or dismount was needed in the tests. If writes are refused
  on some system, `FSCTL_LOCK_VOLUME` is the first thing to try (untested).
- Speed is that of USB full speed: about 750 kB/s reading, 660 kB/s writing.
  A whole Zip image takes over two minutes each way. Read and write only the
  sectors that are needed.

## Rules that protect the data

1. **Take only while the SCSI host is idle.** `@take` does not ask the host.
   If the host is loading or saving, it is the same as pulling the disk out
   of the drive. The tool cannot see what the host is doing, so ask the user.
2. **Give back promptly.** The host has no disk between take and give. Do the
   work in one go; do not hold the media while waiting for user input.
3. **Flush and close the disk before `@give`.** Anything the operating system
   has not sent by then never arrives.
4. **A write that succeeded can still fail later.** The firmware acknowledges
   a write when it is in its buffer and writes it to the card up to 20 ms
   afterwards. If the card rejects it, a later write fails, or `@give`
   answers `@ERR give ... reason=write-failed` and the media stays with the
   PC. In both cases treat everything written since `@take` as doubtful:
   write it again, verify by reading back, then `@give` again.
5. **Do not cache across a give.** After `@give` the SCSI host may change the
   disk. Whatever the tool read before must be read again after the next
   `@take`.
6. **Always give back**, also when the tool fails halfway or the user
   cancels. If the tool dies with the media taken, the next start finds
   `owner=usb` in `@status` and should offer to give it back.
7. **Do not power the board off between a write and `@give`.**

## When things go wrong

| What happens | What it means | What to do |
|---|---|---|
| No reply within 15 s | Board busy with a long SCSI transfer, wrong port, or not the handoff firmware | Send `@status` once more; then report |
| `@ERR take ... reason=<r>` | See the reason table in the protocol file | Report the reason in words; do not retry in a loop |
| Media does not appear within 10 s of `@OK take` | Windows has not noticed, or the take was undone | `@status`; if `owner=usb`, keep polling; if `owner=scsi`, start over |
| A read or write fails | SD card error, or the media was revoked | Close the disk, read the serial port for `@EVENT`, `@status`, start over from take |
| `@EVENT revoke id=<id> reason=<r>` | The firmware took the media back: card removed, or another image loaded for that ID | Stop using the disk at once. Unsaved changes are lost. Tell the user |
| `@EVENT io-error id=<id> count=<n>` | An SD card access failed | Treat the session's writes as doubtful |
| `@OK give ... forced=1` | Something was still reading or writing 5 s after `@give` was sent | Warn. Normally this means the disk handle was not closed first |
| `@ERR give ... reason=write-failed` | Data did not reach the card; media is still with the PC | Write again and verify, or tell the user, then `@give` again |
| Serial port disappears | Board unplugged or restarted | After a restart the media is with the SCSI host again; start over |

None of the failure paths could be provoked in the bench tests, because no
SD card access failed. They are implemented and were read through, not
exercised.

## What to tell the user

- Before take: "Make sure the sampler is not loading or saving. The disk will
  be removed from it until the changes are written."
- After give: whatever turns out to be needed on the sampler to see the
  changes. **Not known yet**; see the next section.
- `reason=not-contiguous`: "The image file is fragmented on the SD card.
  Copy it to a freshly formatted card."
- `reason=disabled`: "USBMediaHandoff = 1 is missing in zuluscsi.ini, or the
  card holds no removable-type image (file name starting with ZP, RE, MO or
  FD)."
- `ro=1`: the image file is read-only on the card; writes will fail.

## Not known yet

These need the sampler. The procedure is in
[05-sampler-test.md](05-sampler-test.md); the answers decide what the tool
should do or say.

1. Does the sampler refuse to load and save while the media is taken, and
   what does it show?
2. After `@give`, does the sampler see changed content by itself, after
   re-reading the directory, after selecting the storage device again, or
   only after a restart?
3. Does the sampler cope with being told "disk changed" (the UNIT ATTENTION
   that `EnableUnitAttention = 1` produces), or does it work better without
   that setting?
4. Does raw access through the drive letter work with a real Ensoniq image
   as it did with the test pattern?
5. Does the tool's existing raw-disk code work on this disk unchanged?

## Before calling it done

- [ ] The tool finds port and disk by USB identity, not by fixed names
- [ ] It refuses to send anything to a port that does not have ID `2E8A:10D7`
- [ ] Commands end with CR; the port is never opened at 1200 baud
- [ ] `@status` is checked before `@take`, and each `unavailable` reason and
      `@ERR` reason is shown in words
- [ ] It waits for the media by size, with a timeout
- [ ] The disk handle is flushed and closed before `@give`
- [ ] `@give` is sent on every exit path, including errors and cancel
- [ ] A start with `owner=usb` is handled
- [ ] `forced=1`, `write-failed`, `revoke` and `io-error` are handled
- [ ] A change made by the tool is visible on the sampler, and a change made
      on the sampler is visible in the tool after the next take
- [ ] Ten take, edit, give cycles in a row without an error

## Reporting back

Write what you find into [06-field-reports.md](06-field-reports.md), in this
repository. The template is at the top of that file. Wanted in particular:
the answers to "Not known yet", anything in these files that turned out to
be wrong, and anything the firmware should do differently to make the tool
simpler or safer.
