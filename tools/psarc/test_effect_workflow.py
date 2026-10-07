#!/usr/bin/env python3
import json
from pathlib import Path


def main():
    preset_path = Path("tools/psarc/default_bass_tone.json")
    assert preset_path.exists(), "Bundled bass preset is missing"
    data = json.loads(preset_path.read_text(encoding="utf-8"))
    attrs = next(iter(data.get("Entries", {}).values()))["Attributes"]
    tones = attrs.get("Tones", [])
    assert len(tones) == 1, "Expected one source bass tone template"
    gear = tones[0]["GearList"]
    assert gear["Amp"]["Key"] == "Bass_Amp_EdenWT800"
    assert gear["Cabinet"]["Key"] == "Bass_Cab_AT810BC_Tube_Edge"
    assert gear["PrePedal1"]["Key"] == "Bass_Pedal_MBComp"
    assert gear["PostPedal1"]["Key"] == "Bass_Pedal_BassEQ8"

    export = Path("src/psarc_export.c").read_text(encoding="utf-8")
    compat = Path("src/psarc_export_null_compat.h").read_text(encoding="utf-8")
    stick = Path("src/psarc_stick_sample_hook.h").read_text(encoding="utf-8")
    stick_v2 = Path("src/psarc_stick_sample_v2_hook.h").read_text(encoding="utf-8")
    dialog = Path("src/psarc_dialog.c").read_text(encoding="utf-8")
    song_h = Path("src/song.h").read_text(encoding="utf-8")
    song_c = Path("src/song.c").read_text(encoding="utf-8")
    hq = Path("src/tone_analysis_hq.c").read_text(encoding="utf-8")
    workflow = Path("src/tone_workflow.c").read_text(encoding="utf-8")
    helper = Path("tools/psarc/eof_psarc_helper.cs").read_text(encoding="utf-8")
    hot_preview = Path("tools/psarc/eof_psarc_hot_preview.cs").read_text(encoding="utf-8")
    packer = Path("tools/psarc/eof_psarc_packer_override.cs").read_text(encoding="utf-8")
    builder = Path("tools/psarc/build_helper.ps1").read_text(encoding="utf-8")

    assert 'track == EOF_TRACK_DRUM_DTX' in export
    assert 'default_bass_tone.json' in export
    assert 'for(i = 0; i < 2; i++)' in export
    assert 'if(bass && !row->source[0] && have_basspreset)' in export

    # The helper may use unique Tone2014.Key values internally while building
    # DLCPackageData.  Before serialization the packer adapter must convert every
    # arrangement slot/XML reference to the matching Tone2014.Name, because the
    # RSToolkit RS2014 manifest builder resolves tones by Name, not by Key.
    assert 'NormalizeRocksmithToneSlots(info);' in packer
    assert 'NormalizeRocksmithToneNames(info);' in packer
    assert 'ToneReferenceMatches' in packer
    assert 'arrangement.ToneBase = display;' in packer
    assert 'arrangement.ToneA = display;' in packer
    assert 'RewriteArrangementXmlToneNames(arrangement.SongXml.File, rename);' in packer
    assert 'tone.Name = display;' in packer
    assert 'Rocksmith tone binding:' in packer
    assert 'SmartTitleCase' in packer
    assert 'LowerCaseTitleWords' in packer
    assert '" - " + SmartTitleCase(effect)' in packer
    assert '"de", "da", "do", "das", "dos"' in packer

    # Exact requested Rocksmith arrangement mapping, including the added Alt Lead.
    assert 'case EOF_TRACK_PRO_GUITAR: return "rhythm";' in export
    assert 'case EOF_TRACK_PRO_GUITAR_22: return "lead";' in export
    assert 'case EOF_TRACK_PRO_GUITAR_B: return "alt_rhythm";' in export
    assert 'case EOF_TRACK_PRO_GUITAR_22_BONUS: return "alt_lead";' in export
    assert 'EOF_TRACK_PRO_GUITAR_22_BONUS' in song_h
    assert 'PART_REAL_GUITAR_22_BONUS' in song_c
    assert 'role == "alt_rhythm" || role == "alt_lead"' in helper
    assert 'role == "lead" || role == "alt_lead"' in helper

    # Preview now mirrors the DTX intelligent selector: 25-second PCM RMS windows
    # every five seconds, keeping the first hottest window and applying no fades.
    assert 'PreviewMilliseconds = 25000L' in hot_preview
    assert 'StepMilliseconds = 5000L' in hot_preview
    assert 'WindowEnergy' in hot_preview
    assert 'offset < musicBytes - windowBytes' in hot_preview
    assert 'energy > bestEnergy' in hot_preview
    assert 'PSARC preview: DTX hottest 25s RMS window, no fades' in hot_preview
    assert 'HotPreview.Create(spec, wav, previewWav);' in builder
    assert 'eof_psarc_hot_preview.cs' in builder
    assert 'afade' not in hot_preview

    # Bundled TuxGuitar drumstick sample replaces the synthetic square-wave
    # count-in.  V2 restores normal Target-ms handling and positions one meter of
    # clicks so it ends exactly at the first chart beat, using initial tempo and
    # the predominant time signature.
    sample = Path("tools/psarc/drumstick.wav")
    assert sample.exists(), "Bundled drumstick.wav is missing"
    raw = sample.read_bytes()
    assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE", "drumstick.wav is not RIFF/WAVE"
    assert 'eof_psarc_stick_initial_quarter_ms' in stick
    assert 'eof_psarc_stick_predominant_meter' in stick
    assert '#undef strtoul' in stick_v2
    assert 'downbeat_ms' in stick_v2 and 'measure_ms' in stick_v2
    assert 'EOF_PSARC_DRUMSTICK_SAMPLE_NAME "drumstick.wav"' in stick_v2
    assert 'eof_psarc_stick_v2_mix_to_downbeat' in stick_v2
    assert 'eof_psarc_internal_ogg_to_wav_compat' in stick_v2

    # Internal WAV staging still uses EOF's linked Vorbis decoder.  No real
    # FluidSynth/SoundFont process is required for the sampled count-in.
    assert '#include <alogg.h>' in compat
    assert 'EOF_PSARC_INTERNAL_AUDIO' in compat
    assert 'alogg_create_ogg_from_file' in compat
    assert 'alogg_partial_read' in compat

    # Installed RSToolkit runtime and known-good Python-style packer flow.
    assert 'Select RocksmithToolkitGUI.exe from the installed RSToolkit folder' in dialog
    assert '"psarc", "rstoolkit_root"' in dialog
    for name in ['RocksmithToolkitLib.dll', 'ICSharpCode.SharpZipLib.dll', 'MiscUtil.dll',
                 'Newtonsoft.Json.dll', 'X360.dll', 'zlib.net.dll']:
        assert "'" + name + "'" in builder
    assert 'new DataContractSerializer(typeof(DLCPackageData)).WriteObject(writer, info);' in packer
    assert 'Path.Combine(toolkitRoot, "packer.exe")' in packer
    assert 'Process.Start(psi)' in packer
    assert 'info.SignatureType = PackageMagic.CON;' in packer

    # PC/Mac only, both enabled by default and built independently.
    assert '#define EOF_PSARC_PLATFORM_PC' in dialog
    assert '#define EOF_PSARC_PLATFORM_MAC' in dialog
    assert 'Generate PC PSARC' in dialog
    assert 'Generate Mac PSARC' in dialog
    assert 'static int eof_psarc_platform_pc = 1;' in dialog
    assert 'static int eof_psarc_platform_mac = 1;' in dialog
    assert 'EnvironmentFlag("EOF_PSARC_PC", true)' in packer
    assert 'EnvironmentFlag("EOF_PSARC_MAC", true)' in packer
    assert 'info.XBox360 = false;' in packer and 'info.PS3 = false;' in packer
    assert 'stem + "_p.psarc"' in packer and 'stem + "_m.psarc"' in packer

    assert 'eof_tone_analysis_hq_track_has_results' in export
    assert 'Analyze guitar effects' in export
    assert 'eof_tone_analysis_hq_estimate' in export
    assert '#define HQ_RATE 22050UL' in hq
    assert '#define HQ_FFT 8192U' in hq
    assert '#define HQ_STFT_HOP 512U' in hq
    assert '#define HQ_AUTOFX_VECTOR 163U' in hq
    assert '#define HQ_MFCCS 10U' in hq
    assert 'AI Distortion' in hq
    assert 'eof_tone_analysis_hq_menu();' in workflow

    notice = Path("src/AUTOFX_NOTICE.txt")
    assert notice.exists()
    notice_text = notice.read_text(encoding="utf-8")
    assert 'Sony CSL Paris' in notice_text and 'MIT License' in notice_text

    print("PSARC workflow test OK: sampled count-in, tone-name binding, hottest no-fade preview, PC/Mac and Rhythm/Lead/Alt arrangement mapping verified.")


if __name__ == "__main__":
    main()
