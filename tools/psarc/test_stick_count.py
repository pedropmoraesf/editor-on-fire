#!/usr/bin/env python3
import math
import os
import struct
import subprocess
import tempfile
from pathlib import Path

PPQ = 480
NOTE = 37
CHANNEL = 9  # MIDI channel 10, zero-based


def predominant_bpm(beat_positions_ms):
    """Mirror eof_psarc_predominant_bpm() for a deterministic integration test."""
    histogram = [0] * 301
    best = 120
    best_count = 0
    for a, b in zip(beat_positions_ms, beat_positions_ms[1:]):
        length = b - a
        if length <= 0:
            continue
        bpm = int(60000.0 / length + 0.5)
        while bpm < 60:
            bpm *= 2
        while bpm > 240:
            bpm //= 2
        if 30 <= bpm <= 300:
            histogram[bpm] += 1
            if histogram[bpm] > best_count:
                best_count = histogram[bpm]
                best = bpm
    return best


def vlq(value):
    out = [value & 0x7F]
    value >>= 7
    while value:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    return bytes(reversed(out))


def make_midi(path, bpm, duration_ms):
    beat_ms = 60000.0 / bpm
    beats = max(1, int(math.ceil(duration_ms / beat_ms)))
    mpqn = int(60000000.0 / bpm)
    tr = bytearray()
    tr += b"\x00\xFF\x51\x03" + bytes([(mpqn >> 16) & 0xFF, (mpqn >> 8) & 0xFF, mpqn & 0xFF])
    for i in range(beats):
        tr += vlq(0 if i == 0 else 420)
        tr += bytes([0x90 | CHANNEL, NOTE, 112])
        tr += vlq(60)
        tr += bytes([0x80 | CHANNEL, NOTE, 0])
    tr += vlq(420) + b"\xFF\x2F\x00"
    data = b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQ)
    data += b"MTrk" + struct.pack(">I", len(tr)) + tr
    Path(path).write_bytes(data)
    return beats


def read_vlq(data, pos):
    value = 0
    while True:
        b = data[pos]
        pos += 1
        value = (value << 7) | (b & 0x7F)
        if not (b & 0x80):
            return value, pos


def inspect_midi(path):
    data = Path(path).read_bytes()
    assert data[:4] == b"MThd"
    length, fmt, tracks, division = struct.unpack(">IHHH", data[4:14])
    assert length == 6 and fmt == 0 and tracks == 1 and division == PPQ
    assert data[14:18] == b"MTrk"
    size = struct.unpack(">I", data[18:22])[0]
    track = data[22:22 + size]
    pos = 0
    tick = 0
    note_ons = []
    tempo = None
    end_tick = None
    while pos < len(track):
        delta, pos = read_vlq(track, pos)
        tick += delta
        status = track[pos]
        pos += 1
        if status == 0xFF:
            kind = track[pos]
            pos += 1
            length, pos = read_vlq(track, pos)
            payload = track[pos:pos + length]
            pos += length
            if kind == 0x51:
                tempo = int.from_bytes(payload, "big")
            elif kind == 0x2F:
                end_tick = tick
                break
        elif status & 0xF0 in (0x80, 0x90):
            note = track[pos]
            vel = track[pos + 1]
            pos += 2
            if status & 0xF0 == 0x90 and vel:
                note_ons.append((tick, status & 0x0F, note, vel))
        else:
            raise AssertionError(f"Unexpected MIDI status 0x{status:02X}")
    return tempo, note_ons, end_tick


def probe_duration(path):
    result = subprocess.run([
        "ffprobe", "-v", "error", "-show_entries", "format=duration",
        "-of", "default=nw=1:nk=1", str(path)
    ], check=True, capture_output=True, text=True)
    return float(result.stdout.strip())


def main():
    source = Path("src/psarc_export.c").read_text(encoding="utf-8")
    for token in ("eof_psarc_predominant_bpm", "MIDI_BYTE(37)", "420UL", "60UL", "0x99", "0x89", "0xE0"):
        assert token in source, f"Expected stick-count implementation token missing: {token}"

    # Six 500 ms intervals dominate a single 600 ms tempo outlier.  The same
    # histogram logic used by EOF therefore has to choose 120 BPM, not 100 BPM.
    beat_positions = [0, 500, 1000, 1500, 2000, 2600, 3100, 3600, 4100]
    bpm = predominant_bpm(beat_positions)
    assert bpm == 120, bpm

    duration_ms = 3000
    with tempfile.TemporaryDirectory() as td:
        midi = Path(td) / "countin.mid"
        wav = Path(td) / "countin.wav"
        trimmed = Path(td) / "countin_trimmed.wav"
        beats = make_midi(midi, bpm, duration_ms)
        tempo, note_ons, end_tick = inspect_midi(midi)
        assert beats == 6
        assert tempo == 500000
        assert [x[0] for x in note_ons] == [0, 480, 960, 1440, 1920, 2400]
        assert all(ch == CHANNEL and note == NOTE for _, ch, note, _ in note_ons)
        assert end_tick == 2880

        sf2 = os.environ.get("EOF_TEST_SF2", "")
        if not sf2:
            candidates = list(Path("/usr/share/sounds").rglob("*.sf2")) if Path("/usr/share/sounds").exists() else []
            sf2 = str(candidates[0]) if candidates else ""
        assert sf2 and Path(sf2).exists(), "No GM SoundFont available for FluidSynth integration test"
        assert subprocess.call(["which", "fluidsynth"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0
        subprocess.run(["fluidsynth", "-ni", sf2, str(midi), "-F", str(wav), "-r", "44100"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        assert wav.exists() and wav.stat().st_size > 44
        rendered = probe_duration(wav)
        assert rendered >= 3.0, rendered

        # EOF trims FluidSynth's possible release/reverb tail to prefix_ms before
        # concatenating the source song.  Validate that exact 3000 ms boundary.
        subprocess.run([
            "ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", str(wav),
            "-af", "atrim=duration=3.000,asetpts=PTS-STARTPTS", str(trimmed)
        ], check=True)
        exact = probe_duration(trimmed)
        assert 2.99 <= exact <= 3.01, exact

    print("Stick count test OK: predominant tempo 120 BPM selected over outlier; 6 GM side-stick hits rendered and trimmed to 3000 ms.")


if __name__ == "__main__":
    main()
