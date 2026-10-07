#!/usr/bin/env python3
from pathlib import Path


def require(text, needle, label):
    assert needle in text, f"Missing {label}: {needle}"


def main():
    hook = Path("src/dtx_gp_import_hook.h").read_text(encoding="utf-8")
    impl = Path("src/dtx_xml.c").read_text(encoding="utf-8")
    header = Path("src/dtx_xml.h").read_text(encoding="utf-8")
    makefile = Path("src/makefile.common").read_text(encoding="utf-8")
    file_menu = Path("src/menu/file.c").read_text(encoding="utf-8")

    # The ordinary EOF Save/Quick Save path must still call eof_save_song(), and
    # menu/file.c must be the translation unit where that call is redirected.
    require(file_menu, "eof_save_song(eof_song, eof_temp_filename)", "EOF project-save call")
    require(hook,
            "#define eof_save_song(sp, fn) eof_dtx_save_song_hook((sp), (fn))",
            "save hook redirection")
    require(makefile,
            "menu/file.o: menu/file.c dtx_gp_import_hook.h dtx_integration.h dtx_xml.h",
            "menu/file hook dependency")
    require(makefile, "dtx_xml.o", "DTX XML object in build")

    # The hook must call the real save first and only then produce the sidecar.
    require(header, "int eof_dtx_save_song_hook(EOF_SONG *sp, const char *filename);",
            "DTX save hook declaration")
    require(impl, "int result = eof_save_song(sp, filename);", "real EOF save invocation")
    require(impl,
            'replace_filename(path, project_filename, "PART_REAL_DRUM_DTX.xml", sizeof(path));',
            "sidecar path")
    require(impl, '<partRealDrumDtx version=', "XML root element")
    require(impl, '<track name=\\\"PART_REAL_DRUM_DTX\\\">', "DTX track element")
    require(impl, '<hit timeMs=', "DTX hit serialization")
    require(impl, "eof_dtx_integration_channel_from_midi", "DTX MIDI/channel mapping")

    # Emergency backup/clone saves must not overwrite the active project's
    # sidecar; normal .eof Save and Save As do.
    require(impl, "if(sp != eof_song || (name && strstr(name, \"lostoggbackup\")))",
            "backup/clone guard")
    require(impl, "if(ustricmp(get_extension(filename), \"eof\"))",
            "EOF extension guard")

    print("DTX XML save test OK: normal EOF saves are wired to generate PART_REAL_DRUM_DTX.xml after the project save.")


if __name__ == "__main__":
    main()
