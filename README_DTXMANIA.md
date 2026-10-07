# DTXMania support added to Editor On Fire

This build adds a dedicated `PART_REAL_DRUM_DTX` track under **Song > Track** and a **File > Export > Export DTXMania** command.

All dialogs and messages introduced by this DTXMania integration are intentionally written in English so the feature can be used in an international fork of EOF.

## PART_REAL_DRUM_DTX representation

The DTX track intentionally uses EOF's pro-guitar note storage as a carrier for percussion data. Each used string's **fret value stores a General MIDI percussion note number**. The track has a fret limit of 127 so the original Guitar Pro percussion identity can be retained instead of collapsing the imported chart to five generic drum lanes.

Guitar Pro percussion imports are routed directly to `PART_REAL_DRUM_DTX`. The imported source is treated as the maximum DTX difficulty, **ULT**, and ULT becomes the active difficulty whenever the user enters this track.

The five DTX tabs are:

- BSC — BASIC
- ADV — ADVANCED
- EXT — EXTREME
- MAS — MASTER
- ULT — ULTIMATE

When five-level generation is enabled, missing lower difficulties are generated from ULT and materialized as real EOF notes in those tabs. Once a difficulty contains notes, subsequent exports use the notes currently stored in that tab instead of replacing them with a newly generated version. This allows the generated chart to be edited normally in EOF.

## DTX drum channels

The playable drum channels below are based on the DTXMania channel model used by the DTXManiaNX source (`EChannel.cs`). The original Python exporter covered channels 11 through 19. The EOF implementation additionally separates the DTX-native left cymbal, left pedal and left bass drum lanes.

| DTX channel | DTXMania lane | General MIDI values accepted by EOF |
| --- | --- | --- |
| `11` | Closed hi-hat | 42 |
| `12` | Snare | 37, 38, 39, 40, 62 |
| `13` | Bass drum | 31, 36 |
| `14` | High tom | 48, 50 |
| `15` | Low tom | 45, 47 |
| `16` | Cymbal / right crash family | 52, 55, 57 |
| `17` | Floor tom | 41, 43 |
| `18` | Open hi-hat | 46 |
| `19` | Ride cymbal | 51, 53, 59 |
| `1A` | Left cymbal | 49 |
| `1B` | Left pedal | 44 |
| `1C` | Left bass drum | 35 |

DTXMania also defines hidden/no-chip variants of the drum lanes and control channels such as `1F` (drum fill), `54` (movie), BGA layers, beat-line controls and sound-effect channels. These are not additional physical drum pieces, so EOF does not misrepresent them as separate drum instruments. The gameplay-video exporter uses DTX movie channel `54` when a video is supplied.

If Guitar Pro contains a percussion MIDI number that has no DTXMania drum mapping, EOF keeps the note in `PART_REAL_DRUM_DTX`, highlights it and reports the MIDI number to the user instead of silently remapping or deleting it. Unsupported pieces are not exported as ordinary DTX drum chips until a mapping is defined.

## DTX editor colors and notation

`PART_REAL_DRUM_DTX` keeps EOF's familiar drum notation: circular symbols for drums and triangular symbols for cymbals. The DTX-specific colors are:

- kick: white
- snare: gold
- closed/open hi-hat: blue
- ride: light blue
- tom 1: green
- tom 2: red
- tom 3/floor tom: purple
- crash/cymbal: light gray
- unsupported percussion: orange with a yellow highlight and the MIDI number displayed

## Export dialog

**File > Export > Export DTXMania** opens **DTXMania Export Settings**. Song title and artist/author are prefilled from EOF metadata and are mandatory. The dialog also provides:

- latency compensation in milliseconds, default `250`; the dialog explains that this value is intended to compensate Bluetooth audio latency when playing electronic drums
- loudness normalization, default enabled
- LUFS target, default `-14`
- five-level DTX generation
- jacket / cover image (`PREIMAGE`)
- preview movie (`PREMOVIE`)
- gameplay video (`AVI` + movie channel `54`)
- stage image (`STAGEFILE`)
- result image (`RESULTIMAGE`)

The selected optional media files are copied into the generated song folder and referenced by the DTX files.

## Output folder

The user chooses the parent output directory. EOF always creates a child directory named:

`Artist - Song Title`

Windows-invalid filename characters are sanitized. If the output-folder chooser is cancelled, the child directory is created in the same directory that contains the current `notes.eof` project. It no longer falls back to Downloads.

## Audio, normalization and preview

The exporter first tries the FFmpeg executable configured in EOF, then a local `ffmpeg.exe`, then `ffmpeg` available through the system PATH.

If FFmpeg is available, EOF can apply `loudnorm` normalization and creates a 25-second OGG preview with fades. If FFmpeg is not available, export is **not blocked**: EOF can continue without normalization and copies the exported song OGG to `preview.ogg`, which gives DTXMania a valid preview target instead of failing the entire export.

The FFmpeg prompt offers **Locate FFmpeg**, **Continue** and **Cancel** instead of forcing the user to browse for `ffmpeg.exe`.

## DTX media/directive support used by the exporter

The generated DTX files use the following relevant directives when applicable:

- `TITLE`, `ARTIST`, `COMMENT`, `BPM`, `DLEVEL`, `DIFFICULTY`
- `PREVIEW`
- `PREIMAGE`
- `PREMOVIE`
- `STAGEFILE`
- `RESULTIMAGE`
- `WAVxx`, `BGMWAV`
- `AVIxx` and channel `54` for gameplay video
- channel `02` measure-length multipliers
- drum channels `11` through `1C`

DTXMania supports additional BGA scripting, hidden/no-chip channels, mixer/SE channels and other low-level chart controls. They are format features rather than additional user media attachments or physical drum pieces and are intentionally not emitted unless the EOF exporter has a concrete use for them.

## Difficulty export behavior

When five-level generation is enabled, the exporter writes BASIC, ADVANCED, EXTREME, MASTER and ULTIMATE files and a `set.def`. The generated levels are also placed into the corresponding EOF BSC/ADV/EXT/MAS/ULT tabs, so the user can edit them before exporting again.

On subsequent exports, populated difficulty tabs are treated as authored data and are used directly. The exporter only generates a level when that difficulty is missing.

When exiting EOF with DTX chart data present, EOF asks whether to export to DTXMania before exiting. The message indicates whether the DTX data has changed since the last successful export.

## Building on the Windows/MSYS2 setup

No additional C library dependency was added for DTX export itself. The modern Guitar Pro importer still requires the 32-bit MSYS2 libxml2/zlib packages already used by this project.

From the project root:

```sh
git pull
make clean
make
```
