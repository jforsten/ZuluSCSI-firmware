# Test with the sampler

The first test of the USB media handoff with a SCSI host attached. Everything
so far was tested over USB only. This procedure answers the open questions in
[04-app-integration.md](04-app-integration.md), "Not known yet", and checks
the one safety property that could not be tested on the bench: that the
sampler cannot read or write the image while the PC has it.

Write the results into [06-field-reports.md](06-field-reports.md), using the
test numbers below.

## What you need

- ZuluSCSI Mini RP2040 with the firmware from [firmware/](firmware/README.md)
  (reports `git:48fce31-dirty`, built `2026-11-28T02:12:46`) or newer.
- Its SD card with an Ensoniq disk image on it (see "Preparing the card").
- The sampler, connected to the board's SCSI connector as for normal use.
- A PC connected to the board's USB port **at the same time**.
- Python 3 with `pyserial` for the tools in [tools/](tools/).
- A way to change the image from the PC: the disk tool, pointed at the USB
  disk the way it is pointed at a SCSI2SD V6 disk. Tests 1 to 3 and 6 do not
  need it.

## Preparing the card

Do this with the card in a card reader.

1. **The image.** One of:
   - Copy an existing Ensoniq image onto the card and name it `ZP0_<name>.img`
     for a Zip 100 drive on SCSI ID 0. Use another digit for another ID.
     A `ZP` image should be exactly 100,663,296 bytes. For another size use
     the prefix `RE` (generic removable disk); whether the sampler accepts
     that drive type is itself a finding.
   - Or keep a blank image of that size on the card and format it from the
     sampler in test 1.

   Only one image per SCSI ID. The test card currently holds
   `ZP0_handoff_test.img`, a number pattern used for the bench tests. Remove
   or rename it (for example to `handoff_test.bak`) if the new image uses
   ID 0, or reuse it as the blank image.

   The file must be in one piece on the card. Copying it onto a card with
   plenty of free space normally achieves that; test 0 checks it.

2. **`zuluscsi.ini`** in the root of the card:

   ```ini
   [SCSI]
   USBMediaHandoff = 1
   EnableUnitAttention = 1
   Debug = 1
   ```

   `Debug = 1` logs every SCSI command, which is what shows how the sampler
   reacts. It slows the board down. If the sampler misbehaves already in
   test 1, repeat test 1 with `Debug = 0` and say so in the report. Remove
   the line after the test.

3. Keep a copy of the image on the PC. Tests 2 and 7 are meant to be
   harmless, but this is a prototype.

## Recording the session

Start this before powering the sampler on and leave it running:

```
python tools/find_device.py
python tools/handoff_console.py COM4 --log sampler-test.log
```

- `find_device.py` prints the serial port and drive letter to use.
- In the console, a line starting with `@` is sent to the board. **Any other
  line is written into the log as a note.** Type what you do and what the
  sampler shows, as you go: `test 2: pressed LOAD, display says ...`. The
  notes get the time of day, like the board's lines, so they can be matched
  afterwards.
- The console holds the serial port. Programs that need the port themselves
  (`handoff.py`, a disk tool that sends `@take`) cannot run at the same time;
  send `@take` and `@give` from the console instead.

After the test, also keep `zululog.txt` from the SD card.

## Tests

Do them in order. For each one, note what the sampler displayed, word for
word.

### 0. Board state

Console: `@status`

Expected: one line per drive with `owner=scsi` and the right `sectors`, then
`@OK status luns=1`.

- `unavailable=not-contiguous`: the image is fragmented; copy it to a freshly
  formatted card.
- `@ERR status reason=disabled`: the setting is missing or the image name
  does not start with `ZP`, `RE`, `MO` or `FD`.

Note the `FW Version` line from the top of `zululog.txt` or from the console
after a power cycle.

### 1. Normal use, media with the sampler

Power the sampler on. Do what you normally do with the disk: boot from it if
that is the setup, list it, load something, save something.

Expected: no difference from the official firmware.

If a blank image is used, format it from the sampler here.

### 2. Sampler must not reach the disk while the PC has it

This is the most important test.

1. Sampler idle. Console: `@take`. Expected: `@OK take id=0 sectors=... ro=0`.
2. On the PC, take a checksum (about two minutes for a Zip image; use the
   `sectors` value from the reply and the drive letter from `find_device.py`):

   ```
   python tools/test_image.py hash \\.\F: --sectors 196608
   ```

   Run it from PowerShell or the Windows command prompt. Git Bash rewrites
   the `\\.\F:` path.

3. On the sampler: try to list the disk, to **load** from it, and to **save**
   to it. Note each message.
4. Take the checksum again.

Expected:

- Every attempt fails on the sampler, with something like "no disk" or
  "drive not ready".
- With `Debug = 1`, the log shows a line of this form for each attempt that
  got as far as a read or write:
  `------ Command <n> refused on ID 0, media is handed to USB`
- **Both checksums are the same.** If they differ, the sampler wrote to the
  image while the PC had it. Stop and report; do not use the prototype for
  real data.

Do not run `test_image.py` with `mark`, `unmark` or `rewrite` on a real
image. They overwrite it with the test pattern.

### 3. Give back without a change

Console: `@give`. Expected: `@OK give id=0`.

On the sampler, try to use the disk again. Note what it takes:

- works at once,
- works after one failed attempt or an error message,
- works after selecting the storage device again,
- works only after restarting the sampler.

### 4. Change made on the PC

1. Sampler idle, showing or having listed the disk's content.
2. Console: `@take`.
3. With the disk tool, make a change that is easy to recognise on the
   sampler: add a file, or rename one. Close the tool.
4. Console: `@give`.
5. On the sampler, look for the change, in this order, and stop at the first
   that shows it: list the disk again; select the storage device again;
   restart the sampler.

This answers what a user has to do after every edit.

### 5. Change made on the sampler

1. Save something new from the sampler.
2. Console: `@take`. With the disk tool, check that the new file is there
   and intact.
3. Console: `@give`.

### 6. Sampler started while the PC has the media

1. Console: `@take`.
2. Switch the sampler off and on.
3. Note what it does without a disk.
4. Console: `@give`. Note whether the sampler finds the disk now, and what
   it takes.

### 7. Give back by ejecting on the PC

1. Console: `@take`.
2. Eject the drive in Windows Explorer.

Expected: `@EVENT give id=0 source=usb-eject`, then `@OK give id=0`, and the
sampler has the disk again as in test 3.

### 8. Repeat

Ten rounds of test 4 with a small change each time. Expected: every change
arrives; no line with `@ERR`, `io-error`, `revoke` or `forced=1` in the log.

### 9. Optional: take during a load

Only with an image you can afford to lose, and not during a save.

1. Start loading something large on the sampler.
2. Console: `@take` while it loads.

Expected: the load fails on the sampler as if the disk had been pulled out.
The board keeps answering `@status`. After `@give` the sampler can use the
disk again. Report whether the sampler recovered or had to be restarted.

### 10. Optional: without the media change message

Set `EnableUnitAttention = 0`, power-cycle the board, and repeat tests 3 and
4. Report which setting the sampler handles better.

## If something goes wrong

- The board stops answering on the serial port: note the last log lines,
  power-cycle it. After a restart the media is with the sampler.
- The media is stuck with the PC (`owner=usb` in `@status`): `@give`. If it
  answers `@ERR give ... reason=write-failed`, send `@give` again; the second
  one hands the media back as it is.
- The image is damaged: restore the copy made in "Preparing the card".

## What to send back

- `sampler-test.log` from the console, with the notes.
- `zululog.txt` from the SD card.
- A report entry in [06-field-reports.md](06-field-reports.md): sampler model
  and operating system version, image name and size, and the result of each
  test number.
