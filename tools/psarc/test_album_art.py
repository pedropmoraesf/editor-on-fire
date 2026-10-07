#!/usr/bin/env python3
import hashlib
import struct
from pathlib import Path

EXPECTED_SHA256 = "69f55fcaaf6ca4daeef88dd428b25a363623ebbbb9b82e00dcb0bf2f0a17efc5"


def main():
    path = Path("tools/psarc/default_album.png")
    assert path.exists(), "Default PSARC album artwork is missing"
    data = path.read_bytes()
    assert len(data) == 199, len(data)
    assert hashlib.sha256(data).hexdigest() == EXPECTED_SHA256, "Default artwork re-encode changed unexpectedly"
    assert data.startswith(b"\x89PNG\r\n\x1a\n"), "Default album artwork is not a PNG"
    assert len(data) >= 24 and data[12:16] == b"IHDR", "PNG IHDR is missing"
    width, height = struct.unpack(">II", data[16:24])
    assert (width, height) == (512, 512), (width, height)

    dialog = Path("src/psarc_dialog.c").read_text(encoding="utf-8")
    helper = Path("tools/psarc/eof_psarc_helper.cs").read_text(encoding="utf-8")
    assert 'tools/psarc/default_album.png' in dialog
    assert 'Album artwork is missing.' in dialog
    assert 'load_bitmap(eof_psarc_album_art, NULL)' in dialog
    assert 'D_HIDDEN' in dialog  # preview is optional when Allegro cannot decode it
    assert 'album_art.*' in helper
    assert 'AlbumArtPath = albumArt' in helper

    print("Album art test OK: lossless 512x512 re-encode of the uploaded white image is present; missing-art guard and optional preview are wired.")


if __name__ == "__main__":
    main()
