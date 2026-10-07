# EOF — Unofficial modified edition (Pedro Paulo Moraes)

This repository contains an independently modified version of **Editor on Fire (EOF)**, originally developed by **T^3 Software and contributors**.

- **Original project:** https://github.com/raynebc/editor-on-fire
- **Modified source:** https://github.com/pedropmoraesf/editor-on-fire
- **DTXMania editing and export documentation:** [README_DTXMANIA.md](README_DTXMANIA.md)
- **License:** [license.txt](license.txt) (BSD 3-Clause). Original copyright notices and license terms are retained. Bundled third-party components may have their own licenses.
- **Experimental Windows build (2026-10-07):** [Download the complete ZIP](https://raw.githubusercontent.com/pedropmoraesf/editor-on-fire/community-downloads/EOF-Community-Windows-2026-10-07.zip) | [build notes and checksums](https://github.com/pedropmoraesf/editor-on-fire/tree/community-downloads). Extract the entire ZIP before running `eof.exe`; this build has not passed complete regression testing.

This is an **unofficial, community-maintained modification**, not an official EOF build. T^3 Software and the original contributors do not endorse this edition. Features are under development; test builds before relying on them for chart authoring.

> **Português:** Esta é uma versão modificada e não oficial do Editor on Fire, com alterações para edição e exportação DTXMania, entre outras funcionalidades. O código original e os avisos de licença foram preservados. O desenvolvimento é independente dos autores originais.

## Major Features, Improvements and Changes

This unofficial community fork expands EOF's capabilities for Rocksmith 2014, DTXMania, Guitar Pro and other music-chart authoring workflows, while introducing new audio analysis, synchronization and automation features.

### 1. DTXMania Integration

A dedicated `PART_REAL_DRUM_DTX` track has been added to EOF, allowing users to create and edit DTXMania drum charts directly.

Key additions include:

- Importing existing DTXMania `.dtx` files.
- Exporting charts directly to DTXMania format.
- Five editable difficulty levels: BASIC, ADVANCED, EXTREME, MASTER and ULTIMATE.
- Automatic generation of missing lower difficulties from ULTIMATE.
- Preservation of manually edited difficulties during subsequent exports.
- General MIDI percussion mapping, including individual hi-hat, snare, kick, tom, crash, ride, left cymbal, left pedal and left bass drum assignments.
- Dedicated drum and cymbal colors and symbols.
- Warnings for unsupported percussion notes instead of silently discarding them.
- Automatic generation of `set.def` files and appropriate DTX metadata.
- Support for jacket images, stage images, result images, preview movies and gameplay videos.
- Adjustable audio latency compensation, defaulting to 250 ms.
- Automatic creation of an `Artist - Song Title` output directory.

The exporter also supports audio normalization and preview generation. A 25-second preview can be selected from the loudest sustained section of the song, rather than simply using the opening seconds.

See [DTXMania documentation](README_DTXMANIA.md) for detailed format and channel information.

### 2. DTX Import, Playback and Drum Audio

The DTX importer has been expanded to handle difficulty-specific charts, tempo maps, audio references and linked patterns.

An optional FluidSynth-based renderer can generate OGG audio for imported drum arrangements.

Low-latency drum preview playback has also been introduced, using a direct PCM sample path to reduce the audible delay associated with MIDI playback.

Additional improvements address track switching, rewind behavior, empty DTX tracks, rendering, project loading and save-state handling.

When a DTX project is saved, EOF can generate a corresponding `PART_REAL_DRUM_DTX.xml` file. The application also checks for modified DTX data when exiting and can prompt the user to export it.

### 3. Advanced Guitar Pro Import

A new **Guitar Pro (Rocksmith Advanced)** import workflow has been introduced.

It provides more control over importing multiple instruments and converting their arrangements into EOF tracks.

Features include automatic guitar, bass and drum track classification; manual destination-track assignment; multi-track import; integration with EOF's fingering optimization; and support for merging imported material with existing tracks.

Optional synchronization information can be loaded from Songsterr JSON or Sonic Visualiser (SVL) files. The interface provides feedback about timing-file validity.

Improvements were also made to imported time signatures, note placement, tempo-map processing and error recovery.

Successful advanced Guitar Pro imports can automatically save the project.

### 4. Native TuxGuitar Support

Native import support has been added for TuxGuitar 2.x `.tg` files.

The importer reads the modern TuxGuitar ZIP-based project format and processes musical tracks, notes, durations and tempo information.

Users can import these files through the Guitar Pro/TuxGuitar import workflow without first converting them into GP5 format.

Older TuxGuitar formats must be resaved using a compatible TuxGuitar version.

### 5. Create New Project from Guitar Pro Audio

A new **File > New from GP Audio** command allows an EOF project to be created from a Guitar Pro file without requiring a prerecorded MP3 or OGG.

The workflow imports the musical arrangements, prepares MIDI data, renders synthesized audio through FluidSynth and creates the project's OGG audio.

This can be useful for composing, transcribing, practicing or creating charts from MIDI-based music.

### 6. Rocksmith 2014 PSARC Export

An experimental Rocksmith 2014 PSARC export workflow has been integrated into EOF.

The exporter is designed to work with an installed Rocksmith Custom Song Toolkit and Wwise environment.

Its features include PC and Mac target options, automatic song metadata preparation, album artwork selection and preview, Rocksmith arrangement generation, tone configuration and optional Dynamic Difficulty processing.

A two-tone workflow has been implemented to simplify arrangements that require a limited number of guitar tones.

Bass arrangements can use a bundled default bass tone preset, while lead, rhythm and alternative arrangements are handled according to their assigned roles.

The export process can also prepare synchronized audio count-ins using silence or drumstick sounds, with timing adjustments applied to the generated arrangement rather than the original EOF project.

**This functionality requires external tools and remains experimental.** See [PSARC backend documentation](tools/psarc/README.md).

### 7. Experimental Guitar Effect Recognition

An experimental audio-analysis system has been introduced to estimate guitar effects used in a song.

It examines musical and spectral characteristics, using information from both the audio recording and the authored guitar tablature.

The analysis is inspired by the signal-processing concepts published in Sony CSL Paris' AutoFX project.

The implementation uses a custom C-based feature extractor and deterministic classifier rather than the original AutoFX trained neural-network model.

The supported effect categories include Dry, Feedback Delay, Slapback Delay, Reverb, Chorus, Flanger, Phaser, Tremolo, Vibrato, Distortion and Overdrive.

The feature can assist with Rocksmith tone preparation, particularly when the original instrument recording or effect settings are unavailable.

Because the analysis may operate on fully mixed audio, its results should be considered suggestions requiring manual verification. No specific recognition-accuracy percentage is guaranteed.

See [AutoFX attribution and license notice](src/AUTOFX_NOTICE.txt).

### 8. Experimental High-Quality AutoSync

A new audio-based note synchronization workflow has been introduced.

It uses spectral onset analysis, SuperFlux-inspired detection, timing constraints and existing chart information to estimate note positions more accurately.

Special attention has been given to drum charts, including repeated hi-hat or ride patterns.

The DTX synchronization logic considers instrument identity and the spacing between consecutive hits to reduce inappropriate merging of nearby notes.

Additional changes improve synchronization stability when Songsterr or Guitar Pro timing information is already available.

This feature is experimental and may still require manual adjustment, especially with dense arrangements, mixed recordings or live performances.

### 9. Experimental Spectrogram

An additional spectrogram visualization mode has been developed for analyzing musical recordings.

It includes perceptually oriented frequency representation, adaptive contrast, improved high-frequency visibility and cached rendering intended to accelerate repeated visualization.

The experimental display is kept separate from EOF's existing spectrogram configuration.

It is intended to help with note placement, timing adjustments and general audio inspection.

### 10. Audio Normalization and Preview Improvements

Adjustable LUFS audio normalization has been integrated into several workflows.

Users can normalize project audio and configure normalization during new-project creation. The system also supports normalization of temporary audio prepared for DTXMania and PSARC export.

The default target is -14 LUFS, with user-adjustable settings where provided.

Fallback handling has been improved so that certain normalization or encoder failures do not unnecessarily destroy the original audio or prevent other operations from continuing.

The DTX preview system also includes native audio analysis and fade processing, with improved handling of external encoders.

Windows scripts have been added to assist with installing supported audio tools.

### 11. User Interface and Workflow Improvements

Several usability improvements have been implemented, including automatic project saving in selected creation/import workflows, improved dialog layouts, additional warnings and clearer error reporting.

DTX track selection and difficulty handling have been revised to behave more consistently with the editor's existing interface.

Rocksmith bass arrangements receive improved Bass and Alt Bass labeling.

A precise grid-start editing workflow has also been added for adjusting the initial beat position.

EOF playback has been modified to continue when the application loses focus, allowing users to interact with other windows without automatically stopping the chart audio.

### 12. Stability, Compatibility and Maintenance

Numerous changes address crashes and inconsistent behavior involving project loading, DTX tracks, transport controls, Guitar Pro imports, audio conversion and resource management.

Additional diagnostics have been introduced to make troubleshooting easier.

The project also includes internal compatibility adjustments, updated integration hooks, improved error checking and automated tests for selected DTX and PSARC functionality.

Some experimental features have been revised or removed during development. In particular, the earlier automatic guitar chord-track generator is **not included in the current published build**.

---

## Current Windows Build

**Experimental Windows build — October 7, 2026**

The application was successfully compiled for 32-bit Windows using GCC/MinGW.

A basic launch test completed successfully. Selected automated tests also passed, but the entire application has not undergone comprehensive regression testing.

Some PSARC integration tests remain incomplete or unsuccessful, and certain functionality requires separately installed dependencies such as FluidSynth, FFmpeg, Wwise and Rocksmith Custom Song Toolkit.

Users are strongly advised to back up their EOF projects before testing this version.

- **[Download Windows ZIP](https://raw.githubusercontent.com/pedropmoraesf/editor-on-fire/community-downloads/EOF-Community-Windows-2026-10-07.zip)**
- **[Build notes and checksums](https://github.com/pedropmoraesf/editor-on-fire/tree/community-downloads)**
- **[Source code](https://github.com/pedropmoraesf/editor-on-fire)**
- **[Original EOF project](https://github.com/raynebc/editor-on-fire)**

This is an independent community fork and is not affiliated with or endorsed by the original EOF developers. The original copyright notices and applicable licenses have been retained.

Feedback, bug reports and testing results are welcome, particularly from Rocksmith and DTXMania chart authors.

---

## Original EOF documentation

*The following material is retained from the upstream README for reference. Its official EOF download instructions do not refer to this community fork.*

EOF is a song editor for Rocksmith, IMMERROCK, Drums Rock, YARG, Clone Hero, Rock Band, Phase Shift, Frets On Fire and similar rhythm games. The aim of EOF is to provide a simple process with which to create songs. Just provide an OGG file and spend a little time designing note charts and EOF will save files in the appropriate format for immediate use with Frets on Fire, Phase Shift, Clone Hero, Performous and similar games.  Combined with other game-specific tools, you can author charts for games such as Rock Band and Rocksmith.


# Getting EOF #

Binaries are provided for Windows and Mac OS on a somewhat regular basis and can be downloaded from here:\
https://ignition4.customsforge.com/eof

Just click the Apple or Windows logo for your OS and download the relevant files.

The Windows version is distributed as an older release candidate (a full, standalone build) and a "hotfix" containing all files that have changed since that release candidate.  To install the latest release of EOF, extract the release candidate to a folder for which you have write permissions (ie. not within "Program Files" or "Program Files (x86)" ), then extract the hotfix into the release candidate folder (the one that contains eof.exe), allowing it to merge folders and replace existing files.  If you do this correctly, EOF should be able launch and you can verify the hotfix build date by opening its Help menu and selecting About.  For any subsequent hotfix, you can just extract over the previous EOF installation and it will include all updates before and since the previously installed hotfix.

The Mac version is distributed as just the current build.  Do make sure to download and install the Utilities offered on the download page, as this will install the capability to convert chart audio from MP3 and encode OGG audio in various scenarios (converting from MP3 or WAV when creating a new chart, re-encoding audio when adding leading silence, etc).


# Building EOF #

If you're using Linux or if you want to experiment with the source code yourself, there are platform specific build instructions for Windows, Mac OS and Linux available here:\
https://github.com/raynebc/editor-on-fire/tree/wiki

MP3 decoding and OGG encoding capabilities will require LAME and Vorbis Tools to be installed manually, otherwise EOF will warn that applicable features are disabled.
