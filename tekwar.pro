# tekwar.pro -- the full JFTekWar (TekWar) game for the Symbian Belle (Nokia E7) port.
#
# The ENTIRE jftekwar game (all 18 game .c, the datascan.c helper, and
# libsmacker for the Smacker cutscenes) under the Symbian GCCE 4.4.1 toolchain,
# driven by a real Qt 4.7.4 baselayer (src/belle_layer.c + src/belle_main.cpp).
# No SDL, no GTK, no OpenGL. Sound FX come from jfaudiolib (multivoc/fx_man) +
# our DevSound driver (driver_belle.c); music is pre-rendered OGG, played through
# the FX path (FX_PlayLoopedAuto -> MV_PlayLoopedVorbis) via the MUSIC_* shim in
# src/belle_music.c -- Belle has no MIDI/OPL2 synth, so teksnd.c's MIDI/CD calls
# are replaced on Symbian (see teksnd.c and belle_music.c). Smacker video
# (src/teksmk.c) plays on Belle too, through the software renderer and the
# demand-feed audio path. No network (mmulti_null.c).
#
# The game busy-loop runs on a QThread inside the Qt GUI app; see src/belle_layer.c
# and src/belle_main.cpp for the threading model.
#
#   docker run --rm -v <host>/jftekwar:/project symbian-belle-buildtools:final /project
#   -> build/JFTekwar.sis

TEMPLATE = app
TARGET = JFTekwar
QT += core gui

# --- Game sources (all of jftekwar/src, mirroring the desktop Makefile
#     GAMEOBJS) ---
SOURCES += \
    src/b5compat.c \
    src/config.c \
    src/datascan.c \
    src/tekcdr.c \
    src/tekchng.c \
    src/tekgame.c \
    src/tekgun.c \
    src/tekldsv.c \
    src/tekmap.c \
    src/tekmsc.c \
    src/tekprep.c \
    src/teksmk.c \
    src/teksnd.c \
    src/tekspr.c \
    src/tekstat.c \
    src/tektag.c \
    src/tektxt.c \
    src/tekver.c

# --- Smacker video decoder (smacker_jftekwar.c #includes smacker.c) ---
SOURCES += \
    libsmacker/smacker_jftekwar.c

# --- Engine core (software renderer only, no editor build.c/config.c) ---
SOURCES += \
    jfbuild/src/a-c.c \
    jfbuild/src/asmprot.c \
    jfbuild/src/baselayer.c \
    jfbuild/src/cache1d.c \
    jfbuild/src/compat.c \
    jfbuild/src/crc32.c \
    jfbuild/src/defs.c \
    jfbuild/src/engine.c \
    jfbuild/src/kplib.c \
    jfbuild/src/mmulti_null.c \
    jfbuild/src/osd.c \
    jfbuild/src/pragmas.c \
    jfbuild/src/scriptfile.c \
    jfbuild/src/smalltextfont.c \
    jfbuild/src/startwin_stub.c \
    jfbuild/src/talltextfont.c \
    jfbuild/src/textfont.c

# --- Platform glue (Qt baselayer: C platform layer + C++ Qt shell + the
#     MUSIC_* shim) ---
SOURCES += \
    src/belle_main.cpp \
    src/belle_layer.c \
    src/belle_music.c

# --- jfaudiolib (sound FX mixer + FX API) + Belle output driver ---
SOURCES += \
    jfaudiolib/src/asssys.c \
    jfaudiolib/src/driver_belle.c \
    jfaudiolib/src/driver_nosound.c \
    jfaudiolib/src/drivers.c \
    jfaudiolib/src/fx_man.c \
    jfaudiolib/src/mix.c \
    jfaudiolib/src/mixst.c \
    jfaudiolib/src/multivoc.c \
    jfaudiolib/src/pitch.c \
    jfaudiolib/src/vorbis.c

# --- Version stamping (mirrors the desktop Makefiles) ---
# On desktop, when git is present each Makefile regenerates ITS OWN
# version-auto.c and compiles it instead of the committed version.c fallback:
# the game defines game_version (startup banner; tekgame.c app_main) via
# src/version.c / src/version-auto.c; the engine defines build_version (BUILD
# banner, engine.c) via jfbuild/src/build-version.c / build-version-auto.c.
#
# The Symbian pipeline stages the project WITHOUT .git, so the version files are
# stamped on the HOST before the container runs (tools/stamp-version.sh,
# .github/workflows/build-belle.yml). These exists() rules pick the stamped file
# when present and fall back to the committed file otherwise -- exactly like the
# desktop Makefiles' git check. Two files, two versions, each in its own tree:
#   src/version.c / src/version-auto.c                 -> game_version
#   jfbuild/src/build-version.c / build-version-auto.c -> build_version
#
# IMPORTANT: unlike desktop (where the game and engine are built by two separate
# Makefiles that never share an object dir), the Symbian qmake -> .mmp -> sbs
# pipeline compiles everything into ONE build dir and names objects after the
# SOURCE BASENAME. Two sources with the same basename would collapse into one .o
# (sbs warning "overriding commands for target ...", then multiple-definition /
# undefined-symbol link errors). The game and engine files therefore carry
# distinct basenames by construction: version(-auto) in src/, build-version(-auto)
# in jfbuild/src/. (jfbuild/src/version.c was renamed to build-version.c so the
# engine never ships a second file named version.c / version-auto.c.)
exists(src/version-auto.c) {
    SOURCES += src/version-auto.c
} else {
    SOURCES += src/version.c
}
exists(jfbuild/src/build-version-auto.c) {
    SOURCES += jfbuild/src/build-version-auto.c
} else {
    SOURCES += jfbuild/src/build-version.c
}

INCLUDEPATH += \
    . \
    src \
    jfbuild/include \
    jfbuild/src \
    jfaudiolib/include \
    jfaudiolib/src \
    libsmacker

DEFINES += \
    USE_POLYMOST=0 \
    USE_OPENGL=0 \
    USE_ASM=0 \
    B_LITTLE_ENDIAN=1 \
    B_BIG_ENDIAN=0 \
    B_ENDIAN_C_INLINE=1 \
    HAVE_VORBIS

symbian: {
    TARGET.UID3 = 0xE123456A
    # SIS package version (App Manager / installer). Independent of the game's
    # own version string, game_version (src/version.c), which the startup
    # console shows. Dotted numerics only (split on "." for the .pkg header).
    VERSION = 1.0.0
    # App icon in the Belle menu. JFTekwar.mif/JFTekwar.mbm are pre-generated
    # from rsrc/game.bmp by mifconv and COMMITTED so the build is reproducible.
    # On symbian-sbsv2 this qmake cannot run mifconv itself
    # (application_icon.prf skips the !symbian-sbsv2 branch and leaves
    # number_of_icons=0), so the files are installed via pkg rules and referenced
    # from the app-info; the mif/mbm names follow the TARGET (JFTekwar) to match
    # RSS_RULES.icon_file. The whole block is conditional: the first build works
    # fine with no icon at all.
    exists(rsrc/icon/JFTekwar.mif) {
        exists(rsrc/icon/JFTekwar.mbm) {
            default_deployment.pkg_postrules += "\"$$_PRO_FILE_PWD_/rsrc/icon/JFTekwar.mif\" - \"!:\\resource\\apps\\JFTekwar.mif\""
            default_deployment.pkg_postrules += "\"$$_PRO_FILE_PWD_/rsrc/icon/JFTekwar.mbm\" - \"!:\\resource\\apps\\JFTekwar.mbm\""
            RSS_RULES.number_of_icons = 1
            RSS_RULES.icon_file = "\\\\resource\\\\apps\\\\JFTekwar.mif"
        }
    }
    TARGET.EPOCSTACKSIZE = 0x14000
    # Heap cap raised 128MB -> 256MB (0x10000000): on-device the process heap
    # (Qt main thread + game thread share it) saturates at the 0x8000000 cap and
    # the game OOM-crashes during death/respawn allocation spikes.
    TARGET.EPOCHEAPSIZE = 0x20000 0x10000000
    # The on-device game-data directory is defined once in src/belle_config.h
    # (BELLE_GAME_DIR). The Symbian build tools cannot carry a quoted define
    # containing a space through qmake -> .mmp -> GCCE, so it lives in a C header
    # rather than here.
    # GCC defaults to gnu89 for .c; the engine's compat.h demands C99.
    MMP_RULES += "OPTION gcce -std=c99"
    # Native Symbian media-framework output via CMMFDevSound DIRECT.
    LIBS += -lmmfdevsound
}
