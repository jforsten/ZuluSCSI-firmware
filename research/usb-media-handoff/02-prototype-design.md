# USB media handoff: prototype design

Companion to [01-findings.md](01-findings.md). Describes what the prototype on
branch `feature/usb-media-handoff` does and how to use it from a PC.

Status: prototype. See "Test status" at the end for what has been verified.

## Idea

A removable-media image (Zip, removable disk, MO, floppy type) is always in
exactly one of two places:

| Owner | SCSI host sees | USB host sees |
|---|---|---|
| SCSI (default at boot) | drive with media | removable drive, no media |
| USB | drive, NOT READY / medium not present | removable drive with media |

Moving the media is a normal eject and insert as far as each host can tell.
There is never concurrent access to the image, so there is nothing to keep
consistent between the two hosts.

SCSI emulation keeps running the whole time. Other SCSI devices on the same
ZuluSCSI are unaffected.

## Enabling

`zuluscsi.ini`:

```ini
[SCSI]
USBMediaHandoff = 1
EnableUnitAttention = 1   ; recommended, so the SCSI host is told about the media change
```

An image takes part when all of these hold:

- device type is Zip 100, removable, MO or floppy (e.g. file name prefix `ZP`),
- sector size is 512 bytes,
- the image is one contiguous block on the SD card and not a ROM,
  copy-on-write or gapped (AS/400) image. A fragmented image file is refused;
  copy the file to a freshly formatted card to make it contiguous.

Each participating image becomes one USB LUN, numbered in SCSI ID order. The
LUN list is fixed at boot.

## Control from the PC

This section is a summary. The full procedure for a PC tool, with exact reply
formats, timing and how to open the disk, is in
[03-pc-tool-protocol.md](03-pc-tool-protocol.md).

### Serial commands

The USB serial port (same cable, composite device) accepts line commands that
start with `@`. Lines end with CR or LF. Replies are written through the
normal log, so each reply is a complete line with the usual `[1234ms] ` prefix;
look for the `@` token.

| Command | Meaning | Replies |
|---|---|---|
| `@status` | List LUNs | one `@LUN lun=0 id=5 owner=scsi sectors=196608 ro=0` per LUN, then `@OK status luns=1` |
| `@take [id]` | Give the media of SCSI ID `id` to USB | `@OK take id=5 sectors=196608 ro=0` or `@ERR take id=5 reason=...` |
| `@give [id]` | Give it back to the SCSI host | `@OK give id=5` or `@ERR give ...` |

`id` may be left out when there is exactly one LUN.

Requests are carried out between SCSI commands (bus free), so a reply can take
as long as the current SCSI command. `@give` additionally waits until USB has
done no read or write for 300 ms (at most 5 s, then it proceeds anyway and says
so with `forced=1`).

Unsolicited lines:

- `@EVENT give id=5 source=usb-eject` when the PC ejected the drive,
- `@EVENT take id=5 source=usb-load` when the PC sent a load command,
- `@EVENT revoke id=5 reason=...` when the firmware took the media away from
  USB because the image changed underneath (image switched, SD card removed).

Reasons for `@ERR`: `disabled`, `no-such-lun`, `bad-id`, `no-image`,
`not-contiguous`, `sector-size`, `no-card`.

### From the USB side

- Ejecting the drive on the PC (SCSI START STOP UNIT with LoEj, start = 0)
  gives the media back to the SCSI host. Which user action sends this depends
  on the OS; test it before relying on it.
- A load command (LoEj, start = 1, e.g. Linux `eject -t`) requests the media.

### Recommended tool sequence

1. Open the COM port, send `@take`, wait for `@OK take`.
2. Wait for the USB disk to report media (capacity becomes non-zero).
3. Open the raw device, write, flush, close.
4. Send `@give`, wait for `@OK give`.

A Python helper that does steps 1, 2 and 4 is in [tools/handoff.py](tools/handoff.py).

## How it is made safe

### Ownership

`image_config_t::usb_hold` is set while USB owns the media. `ejected` is set
with it. `scsiDiskCloseTray()` does nothing while `usb_hold` is set, which
covers every automatic reinsert path (status poll, INQUIRY, START UNIT, eject
button, media menu). The handoff does not call `doPerformEject()`, because
that would switch to the next image on a multi-image device.

On the USB side the callbacks use a snapshot taken at `@take`: first SD
sector, sector count, writable flag. They never touch the `ImageBackingStore`
object or SdFat, only `SD.card()->readSectors()/writeSectors()`. Ownership is a
single flag written last on take and first on give.

If the image is replaced or closed while USB owns it
(`scsiDiskOpenHDDImage()`, `scsiDiskCloseSDCardImages()`,
`image_config_t::clear()`, SD card removed), the handoff is revoked first. The
main loop also checks the invariants every pass.

### SD card bus

USB callbacks run inside `tud_task()`, which the core calls from a 1 kHz timer
interrupt and from `Serial` calls. They can therefore run at almost any point
of the main loop. Two additions to the RP2040 SD driver (`sd_card_sdio.cpp`)
make that safe; both are inactive unless the handoff is enabled:

1. **Bus guard.** Every `SdioCard` operation takes `USB.mutex` for its
   duration. The timer interrupt skips `tud_task()` while the mutex is held,
   and `Serial` calls made from inside an SD operation return without doing
   anything. So a USB callback can only start between SD operations. The
   existing "USB busy" check (`platform_msc_lock_get()`) also reports true
   during an SD operation.
2. **Stream callback suspend.** While a USB callback does its SD I/O, the
   global SCSI stream callback is not called.

Each SD operation is self-contained in this driver (multi-block transfers are
stopped before the call returns), so interleaving whole operations is safe.

### What the SCSI side notices

- A USB read or write blocks the main loop for the duration of one SD
  operation (one USB chunk, a few ms; a slow card can stall longer on writes).
  A SCSI command to another device that arrives in that time waits.
- While any SD operation runs, USB is not serviced. During long uninterrupted
  SD activity USB slows down but is not starved, because operations are short.

## Transfer speed

A storage transfer runs in 4 kB steps (`CFG_TUD_MSC_EP_BUFSIZE`, fixed in the
precompiled framework library). Each step is started by `tud_task()` once the
previous one has finished, and does its SD card access before (read) or after
(write) the USB part, not at the same time.

The USB core calls `tud_task()` from a timer once per millisecond, so each
step waited up to a millisecond for the next tick. `usbHandoffPoll()` now
calls `platform_msc_handoff_task()` on every main-loop pass while USB has
the media, which runs `tud_task()` directly. It is not called while the SCSI
side has the media, so the SCSI path keeps its timing.

Sequential read of 8 MB, Windows 11, 64 kB requests:

| Firmware | Connection | Serial port closed | Serial port open |
|---|---|---|---|
| `a0a0d24` (timer only) | through two USB 2.0 hubs | 640 kB/s | 686 kB/s |
| `a0a0d24` (timer only) | directly on the PC | 650 kB/s | 798 kB/s |
| with `platform_msc_handoff_task()` | directly on the PC | 756 kB/s | 798 kB/s |

With the serial port open the firmware's own serial polling also runs
`tud_task()`, which is why that column was already faster.

Writing stayed at about 540 kB/s on both builds (about 1 MB written in
five runs, so a rough figure). The wait for the timer tick is therefore not
what limits writes. The likely limit is the SD card write done for every
4 kB step, which has not been measured separately.

What is left, not done:

- The open-port figure is still a little higher, so calling the task from
  more places in the main loop would gain a few percent.
- Doing the SD card access for the next step while the current one is on the
  wire (read-ahead, batched writes). Not measured.
- A larger step size needs a rebuilt framework library.
- Hubs between the board and the PC cost about 14% once the firmware is no
  longer the limit.

## Files touched

| File | Change |
|---|---|
| `src/ZuluSCSI_usb_handoff.{h,cpp}` | New. State, take/give/revoke, USB callback backends, serial command parser. |
| `src/ZuluSCSI.cpp` | Start the handoff after images are opened; poll at bus free; revoke on card removal. |
| `src/ZuluSCSI_disk.{h,cpp}` | `usb_hold` flag; guard in `scsiDiskCloseTray()`; revoke hooks. |
| `src/ZuluSCSI_settings.cpp`, `zuluscsi.ini` | `USBMediaHandoff` setting. |
| `lib/ZuluSCSI_platform_RP2MCU/ZuluSCSI_platform_msc.cpp` | Register storage at boot without blocking; route callbacks to the handoff. |
| `lib/ZuluSCSI_platform_RP2MCU/sd_card_sdio.cpp` | Bus guard and stream callback suspend. |
| `lib/ZuluSCSI_platform_RP2MCU/ZuluSCSI_platform.{h,cpp}` | Feature define, hook declarations, serial input routing. |

## Known limitations

- RP2040 PIO SDIO driver only. RP2350 (`SDIO_RP2350` library), SPI SD and GD32
  are not covered; the feature is compiled out there.
- The LUN list is fixed at boot. An image that becomes eligible later needs a
  restart.
- Registering the storage interface at boot reconnects USB once, which adds
  about half a second to start-up when a PC is attached.
- A host that locked the tray (PREVENT MEDIUM REMOVAL) is not asked; `@take`
  removes the media regardless.
- `@take` during a mounted, in-use disk on the SCSI host is the same as
  pulling a Zip disk out. The host should not be loading or saving.
- USB is full-speed (12 Mbit/s), so about 1 MB/s is the ceiling. Measured:
  about 755 kB/s reading. See "Transfer speed" below.
- The handoff module is also linked into the bootloader image, because the
  USB storage callbacks reference it. It adds about 7 KB there; the bootloader
  is 124,448 bytes of its 131,072 byte limit with this change. If that
  headroom is needed, the module has to be kept out of the bootloader link.

## Test plan

Bench (USB only, no SCSI host needed):

1. Boot with handoff enabled. Serial port and an empty removable drive appear.
2. `@status` lists the LUN with `owner=scsi`.
3. `@take`: drive reports media and the right capacity.
4. Read the whole image over USB and compare with the file on the card.
5. Write a pattern to a few sectors, read back, `@give`, `@take`, read again.
6. Eject on the PC: `@EVENT give` appears.
7. Pull the SD card while USB owns the media: `@EVENT revoke`, no crash.

With the sampler:

8. Boot the sampler from the Zip image; normal use while `owner=scsi`.
9. `@take` while idle: sampler reports no disk.
10. Edit the image, `@give`: sampler sees the changed content (note whether
    the storage device must be re-selected).
11. Copy large data over USB to a second image while the sampler loads from
    the first: no load errors.

## Building and flashing

PlatformIO, from the repository root:

```
pio run -e ZuluSCSI_RP2040
```

The image is `.pio/build/ZuluSCSI_RP2040/firmware.uf2`. Put the board into
its UF2 bootloader (serial console `u` then `y`, or hold BOOTSEL while
plugging in) and copy the file to the drive that appears.

Alternatively, update from the SD card: copy
`.pio/build/ZuluSCSI_RP2040/firmware.bin` to the card's root under a name
that starts with `ZuluSCSI_RP2040` and ends with `.bin`. The board's
bootloader installs it at the next power-on. This replaces the main firmware
only; the bootloader already on the board stays.

The build that was tested is kept in [firmware/](firmware/README.md), with
checksums.

## Test card

`tools/test_image.py create <path>` writes a 100 MB Zip image in which every
sector holds its own number, and `verify` checks a file or a raw USB drive
against that pattern. It is not an Ensoniq disk.

## Test status

2026-11-27:

- Test SD card prepared (32 GB FAT32): `zuluscsi.ini` with the two settings
  above, `ZP0_handoff_test.img` (SCSI ID 0, one contiguous extent, pattern
  verified on the card) and the prototype firmware as an SD update file.
  The board it belongs to ran stock firmware 23.09.06 before.

- Builds without errors or new warnings for `ZuluSCSI_RP2040`
  (flash 442,208 bytes, RAM 187,416 bytes). The handoff functions are present
  in the linked firmware.
- `ZuluSCSI_Pico_2` (RP2350, feature compiled out) still builds, and contains
  none of the handoff code. GD32 builds were not tried.
- Installed on a ZuluSCSI Mini RP2040 through the SD card update (from stock
  23.09.06, keeping that bootloader). It boots, reports USB ID 2E8A:10D7
  "ZuluSCSI RP2040" with a serial port and one storage drive.
- Bench tests on Windows 11, USB only, no SCSI host attached. Passed:
  - Test plan 1 to 3: drive appears empty; `@status` shows `owner=scsi`;
    `@take` makes the media appear with 512 byte sectors.
  - Test plan 4: the whole image (196,608 sectors) read over USB matches the
    pattern, twice, at about 650 kB/s. The second pass was done after all the
    other steps, so nothing wrote to the image during the session.
  - `@give` makes the drive report no media; a read in that state is refused.
  - `@status` answered in about 0.3 s while a USB read was running.
  - Test plan 6: "Eject" in Windows produced `@EVENT give id=0
    source=usb-eject` and the media went back to the SCSI side.
  - No `io-error` or `revoke` events in the log.
  - Test plan 5 (write): 2,068 sectors in five runs (first sector, 16
    sectors, across sector 65536, 1 MB, last sector) written over USB at about
    540 kB/s with `tools/test_image.py mark`. After `@give` and `@take` a full
    read showed the marker data in exactly those sectors and every other
    sector unchanged. `unmark` then restored the pattern, and another full
    read after a give/take cycle matched the original image.
- Windows notes: the raw volume `\\.\F:` (drive letter of the USB disk) can be
  read and written without administrator rights while it holds no file system
  Windows recognises; `\\.\PhysicalDriveN` needs them.
- The write test was checked by reading back over USB only. The image file
  was not compared independently in a card reader afterwards.
- Not tested yet: pulling the SD card (7),
  everything with a SCSI host (8 to 11), fragmented images, more than one
  LUN, other operating systems.

2026-11-28, firmware with `platform_msc_handoff_task()`:

- Installed through the USB bootloader (`u`, `y` on the serial console, then
  the `.uf2`). This also replaced the 2023 bootloader with the one from this
  build. The board boots and reports `git:a0a0d24-dirty`, built 00:58.
- `@status`, `@take`, `@give` work as before.
- Whole image read over USB matches the pattern, at 751 kB/s.
- Read speed figures are in "Transfer speed" above.
- `@status` answered in about 0.3 s while a USB read was running.
- No `io-error` or `revoke` events in the log.
- Write test repeated (same 2,068 sectors as test plan 5): markers verified
  by a full read after a give/take cycle, pattern restored and verified by
  another full read. Write speed 538 kB/s with the serial port closed and
  505 kB/s with it open (two-second samples), so unchanged from the first
  build: the faster servicing does not help writes.
- "Eject" in Windows again produced `@EVENT give id=0 source=usb-eject`.

Update this section as steps are verified.
