# Field reports

What was found when the USB media handoff was used outside the bench tests:
with the sampler, and from the disk tool. Anyone using the prototype adds to
this file, including other development sessions working on the PC tool.

The firmware notes (`01` to `05`) are corrected from what is written here, so
the reports are the record and the other files are the current summary.

## How to add a report

- Add a new section at the **end** of this file, using the template below.
  Do not edit earlier reports; if one turned out wrong, say so in a new one.
- Keep observation and interpretation apart. "The display showed DISK NOT
  READY" is an observation; "the sampler polls the drive" is an
  interpretation. Both are welcome, marked as what they are.
- Say what was **not** tried. An untested item that reads as tested is worse
  than a gap.
- Quote log lines and messages exactly. Put long logs in a file next to this
  one, in a folder `reports/`, and link it.
- If a statement in files `01` to `05` is wrong or missing, quote it with its
  file name and say what was seen instead. Leave those files unchanged.
- Do not commit or push in this repository unless its owner asks for it.

## Template

```markdown
## YYYY-MM-DD: <one-line summary>

- **Who**: <person, or which development session>
- **Firmware**: <the "FW Version" line from the log>
- **Board**: <e.g. ZuluSCSI Mini RP2040>
- **SCSI host**: <sampler model, operating system version; or "none">
- **PC**: <operating system; tool and version or commit>
- **Card setup**: <image file name and size; zuluscsi.ini settings that differ from 05-sampler-test.md>

### Results

| Test (05-sampler-test.md) | Result | What was seen |
|---|---|---|
| 0 Board state | pass / fail / not run | |
| 1 Normal use | | |
| 2 Refusal while taken | | checksums: same / different |
| 3 Give back, no change | | what the sampler needed |
| 4 Change from the PC | | what the sampler needed to show it |
| 5 Change from the sampler | | |
| 6 Sampler started while taken | | |
| 7 Eject on the PC | | |
| 8 Ten rounds | | |
| 9 Take during a load (optional) | | |
| 10 Without unit attention (optional) | | |

### Answers to the open questions (04-app-integration.md, "Not known yet")

1. Sampler refuses load and save while taken:
2. What the sampler needs after give to see a change:
3. With or without EnableUnitAttention:
4. Raw access with a real Ensoniq image through the drive letter:
5. Existing raw-disk code of the tool works unchanged:

### From the tool's side

- What was implemented, and how the board and disk are found:
- Anything in the protocol that was awkward, ambiguous or missing:
- Errors and events actually seen (`@ERR`, `@EVENT`), with the log lines:

### Wrong or missing in the notes

- <file>: "<quoted statement>" - seen instead: ...

### Wishes for the firmware

- ...

### Not tried

- ...

### Attachments

- reports/<file>
```

## Reports

None yet.
