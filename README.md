# Just Intonation Quantizer

A [Disting NT](https://expert-sleepers.co.uk/distingnt.html) community plugin that quantizes CV to a user-defined just intonation scale.

**GUID:** `SFJz`
**Author:** Safie Flato
**Category:** Utility

## Features

- Variable 3–43 note just intonation scale (set via Specifications)
- Note 0 is always 1/1 (hardcoded)
- Notes 1–(N−1) are fully user-defined via numerator/denominator ratios
- Scale is automatically sorted low to high regardless of entry order
- Page labels update live to show the current ratio (e.g. `N1: 3/2`)
- Up to 4 independent CV channels (set via Specifications)
- Per-channel **Input Gate**: quantize only on rising gate edge
- Per-channel **Output Gate**: pass gate through (when Input Gate on), or trigger on note change
- Configurable root offset (±24 semitones)

## Scale Size

The **Notes** specification (3–43, default 12) controls how many notes the scale has. Set it in the Specifications menu before loading the plugin.

### Default scales by note count

**12 notes (default)** — Kraig Grady's [Centaur](http://anaphoria.com/centaur.html) scale:

| Note | Ratio |
|------|-------|
| 0    | 1/1   |
| 1    | 21/20 |
| 2    | 9/8   |
| 3    | 7/6   |
| 4    | 5/4   |
| 5    | 4/3   |
| 6    | 7/5   |
| 7    | 3/2   |
| 8    | 14/9  |
| 9    | 5/3   |
| 10   | 7/4   |
| 11   | 15/8  |

**43 notes** — Harry Partch's 43-tone scale. The Centaur ratios occupy slots 1–11; the remaining 31 Partch ratios fill slots 12–42 in ascending pitch order, giving the complete Partch scale as a starting point.

## Building

Requires the ARM cross-compiler (`arm-none-eabi-g++`) and the distingNT API headers (included as `distingNT_API/`).

```bash
# Hardware build (.o for SD card)
make hardware

# Test build (macOS dylib for nt_emu)
make test
```

Copy `plugins/ji_quantizer.o` to the `plugins/` folder on your Disting NT SD card.

## License

MIT
