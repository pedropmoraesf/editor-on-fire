# EOF Rocksmith 2014 PSARC backend

`File > Export > Rocksmith 2014 PSARC` keeps EOF itself native/32-bit and uses a **complete pre-installed Rocksmith Custom Song Toolkit** as its Rocksmith backend. This follows the same deployment model as the supplied Python generator: EOF is given the RSToolkit installation once, then resolves the library, DDC, Wwise template and `packer.exe` from that installation.

## Metadata and album artwork

The PSARC dialog starts with the project song name and artist converted to uppercase for the initial/default values only. Once the dialog is open, EOF preserves exactly the upper/lower case typed by the user. The default album text is `Live`.

Album artwork is selected with the path field and `Choose image...` button. The repository image `tools/psarc/default_album.png` is the default. When Allegro can decode the selected format, the dialog also displays a small preview; failure to decode the preview does not block export. Pressing OK does block export if the selected/default artwork file no longer exists, so an accidentally deleted default image is reported before PSARC generation starts.

The selected artwork is copied only to the `eof_psarc_tmp` staging folder as `album_art.<extension>`. RSToolkit then receives that file through `DLCPackageData.AlbumArtPath`.

## RSToolkit installation and first export

EOF does not ask the user to locate individual RSToolkit DLLs and does not use a generic "support folder".

On the first PSARC export, EOF asks the user to select an executable from the **main RSToolkit folder**. Allegro 4 has no portable native folder picker, so selecting an executable is how EOF obtains the folder path. Only the containing directory is stored in `[psarc] rstoolkit_root` in `eof.cfg`.

The selected RSToolkit directory is validated against the same installed layout used by the supplied Python generator. It must contain at least:

- `RocksmithToolkitLib.dll`
- `packer.exe`
- `ddc\ddc.exe`
- `Template\Template.wproj`

The helper is compiled **against the DLLs in that installed RSToolkit directory** and is written into the same directory. Running there makes `RocksmithToolkitLib.Extensions.ExternalApps.TOOLKIT_ROOT` point at the real installation, exactly as it does when RSToolkit itself is started. EOF's old bootstrap/worker dependency-redirection design is not used.

## Wwise

The PSARC dialog stores a Wwise **bin directory**, not only an executable. Because Allegro 4 has no portable native folder picker, `Choose bin...` asks the user to select `WwiseCLI.exe` inside the desired bin directory; EOF then stores/displays its parent directory.

Wwise remains a separate installation, just as in the supplied Python script. From the selected Wwise bin directory the helper resolves:

- `WwiseCLI.exe`
- `tools\AkCopyStreamedFiles.exe`

For each export the helper uses the RSToolkit installation directly:

1. Copies the prepared main and preview WAV files to `RSToolkit\Template\Originals\SFX` as `Audio.wav` and `Audio_preview.wav`.
2. Clears the corresponding `Template\.cache\Windows\SFX` output files.
3. Runs `WwiseCLI.exe` on `Template\Template.wproj` with the same SoundBank-generation arguments used by the Python generator.
4. Runs `AkCopyStreamedFiles.exe` against `Template\GeneratedSoundBanks\Windows\SoundbanksInfo.xml`.
5. Reads the generated `Audio*.wem` and `Audio_preview*.wem` files from `Template\.cache\Windows\SFX` and copies them into EOF's staging directory.

When stick count-in is selected, the first use also asks for `fluidsynth.exe` and a General MIDI `.sf2` soundfont. Silence count-in does not require FluidSynth. FFmpeg is required to build the temporary 44.1 kHz stereo WAV; EOF reuses its existing configured FFmpeg path and asks only if that path is missing or invalid.

## Final PSARC packaging

EOF prepares the arrangements, tones, metadata and WEM files through the RSToolkit library, writes a normal RSToolkit `.dlc.xml` template, then invokes the installed command-line packer using the same command pattern as the Python script:

```text
packer.exe -b -t <template.dlc.xml> -o <output.psarc>
```

This keeps the final package-generation step on RSToolkit's own executable instead of maintaining an EOF-specific PSARC packer.

## Guitar effect estimation

The high-quality estimator in `src/tone_analysis_hq.c` is inspired by Sony CSL Paris' MIT-licensed AutoFX project. AutoFX's published feature extractor uses 22.05 kHz audio, an 8192-point spectrogram with 512-sample hop, spectral centroid/spread/skewness/kurtosis, flux, rolloff, slope, flatness, statistics and derivatives, pitch normalization, and 10 MFCCs. Its classifier consumes a 163-value vector.

EOF builds the same conceptual 163-value layout in C and adds full-mix-specific cues for amplitude/spectral modulation, delay autocorrelation and post-note tails. Pitch normalization uses the authored EOF tablature (tuning, fret and capo) instead of trying to infer the guitar pitch from the complete mix.

AutoFX's repository does **not** publish the trained checkpoint/scaler used for its reported classifier result, and its classifier documentation describes single-note effected guitar input. Therefore EOF does not claim AutoFX's reported accuracy on mixed songs. EOF uses its own deterministic classifier over the AutoFX-inspired feature vector and exposes eleven effect families: Dry, Feedback Delay, Slapback Delay, Reverb, Chorus, Flanger, Phaser, Tremolo, Vibrato, Distortion and Overdrive. See `src/AUTOFX_NOTICE.txt` for attribution and license text.

The PSARC `Analyze guitar effects` control only appears for a guitar arrangement that has not already been analyzed through EOF's Tone Change workflow. Bass arrangements instead default to the bundled `tools/psarc/default_bass_tone.json` preset.

## What is reused from RSToolkit

The helper uses RSToolkit's own implementations/files for:

- `Tone2014.Import()` for tone import from RS2014 manifests/JSON, song packages/PSARC and Rocksmith profile databases.
- `ToolkitPedal.LoadFromResource(GameVersion.RS2014)` and `MakePedalSetting()` for the RS2014 pedal database and default knob values.
- `ToneDescriptor.List()` for Rocksmith's descriptor IDs instead of hardcoding those IDs in EOF.
- the installed `ddc\ddc.exe` for optional Dynamic Difficulty generation.
- the installed `Template\Template.wproj` and template directories for Wwise conversion.
- the installed `packer.exe` for final PC RS2014 PSARC generation.

EOF's original project/audio is not rewritten by this pipeline. Rocksmith XML, Dynamic Difficulty changes and count-in timing shifts are made in the `eof_psarc_tmp` staging directory.

## Count-in timing

The dialog's default target is 3000 ms. EOF subtracts the project first-beat/grid offset from that target and only generates the missing prefix. The exact generated prefix is passed to the helper as `prefixMs`; it is not inferred from `songLength` or WAV duration. That prefix is applied to staged `time`, `startTime`, `endTime`, `songLength`, `startBeat` and `offset` values so the WEM and arrangement stay synchronized even when the EOF chart ends before the source audio.

For stick count-in, EOF derives the predominant tempo from consecutive beat intervals, creates a format-0 480-PPQ MIDI using GM Side Stick (note 37, channel 10), renders it with FluidSynth and trims the rendered audio to the exact prefix length before concatenating the song.

## Diagnostics

If the RSToolkit helper, Wwise conversion or packer fails, inspect:

`<project folder>/eof_psarc_tmp/psarc_export.log`

The helper compiler also writes `%TEMP%\eof_psarc_build.log` if it cannot build against the selected RSToolkit installation. The staging folder is deliberately retained after an error so XML, WAV, WEM, generated `.dlc.xml` and helper output can be inspected.
