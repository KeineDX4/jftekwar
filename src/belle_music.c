// belle_music.c -- JFTekWar -> Symbian Belle port: MUSIC_* API shim.
//
// Belle has no MIDI/OPL2 synth (no MT32 / sound card). The game's music is
// therefore pre-rendered to OGG (music/song_<idx>.ogg on the device, next to
// STUFF.DAT; see the M1 music notes) and played straight through the FX/vorbis
// path that the jfaudiolib Belle build already provides: FX_PlayLoopedAuto
// detects the "OggS" magic and hands the data to MV_PlayLoopedVorbis
// (stb_vorbis + the background ring decoder in belle_main.cpp).
//
// teksnd.c talks to music.h; on Belle those calls land here. The music voice is
// a normal FX voice with callbackval = (unsigned int)-1 so teksnd.c's
// soundcallback() -- which indexes dsound[i] WITHOUT an i==-1-style guard for
// any other value -- returns immediately for it. (The reference port uses a
// dedicated MUSIC_ID sentinel and guard; TekWar does not, hence the -1.)
//
// belle_songfile is set by teksnd.c (startmusic/menusong) BEFORE each
// MUSIC_PlaySong call. The buffer handed to FX_PlayLoopedAuto must stay alive
// for the whole playback, so it is owned here and freed on stop/replace.

#include "compat.h"
#include "baselayer.h"
#include "fx_man.h"
#include "music.h"

#ifdef __SYMBIAN32__

#include "belle_music.h"

char belle_songfile[BMAX_PATH+1];

int MUSIC_ErrorCode = MUSIC_Ok;

static int songvoice = -1;
static int musicvolume = 0;
static char *songptr = NULL;
static unsigned int songlen = 0;

static void stopsong(void)
{
    if (songvoice >= 0) {
        FX_StopSound(songvoice);
        songvoice = -1;
    }
    if (songptr) {
        free(songptr);
        songptr = NULL;
        songlen = 0;
    }
}

const char *MUSIC_ErrorString(int ErrorNumber)
{
    (void)ErrorNumber;
    return "Belle has no MIDI synth; music is OGG only";
}

int MUSIC_Init(int SoundCard, const char *params)
{
    (void)SoundCard; (void)params;
    MUSIC_ErrorCode = MUSIC_Ok;
    return MUSIC_Ok;
}

int MUSIC_Shutdown(void)
{
    stopsong();
    MUSIC_ErrorCode = MUSIC_Ok;
    return MUSIC_Ok;
}

void MUSIC_SetVolume(int volume)
{
    musicvolume = volume;
    if (songvoice >= 0)
        FX_SetPan(songvoice, volume, volume, volume);
}

int MUSIC_PlaySong(void *song, unsigned int length, int loopflag)
{
    FILE *f;
    long flen;
    char *buf;

    (void)song; (void)length; (void)loopflag;

    stopsong();

    // Load belle_songfile (relative to the CWD, which tekgame.c chdir()s to
    // BELLE_GAME_DIR) and stream it through the FX/vorbis path, looping.
    f = fopen(belle_songfile, "rb");
    if (!f) {
        buildprintf("belle_music: can't open %s\n", belle_songfile);
        MUSIC_ErrorCode = MUSIC_MidiError;
        return MUSIC_Error;
    }
    fseek(f, 0, SEEK_END);
    flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)flen);
    if (!buf) {
        fclose(f);
        MUSIC_ErrorCode = MUSIC_MidiError;
        return MUSIC_Error;
    }
    if (fread(buf, 1, (size_t)flen, f) != (size_t)flen) {
        fclose(f);
        free(buf);
        MUSIC_ErrorCode = MUSIC_MidiError;
        return MUSIC_Error;
    }
    fclose(f);

    songptr = buf;
    songlen = (unsigned int)flen;

    songvoice = FX_PlayLoopedAuto(songptr, songlen, 0, 0, 0,
                                  musicvolume, musicvolume, musicvolume,
                                  FX_MUSIC_PRIORITY, (unsigned int)-1);
    if (songvoice < 0) {
        buildprintf("belle_music: FX_PlayLoopedAuto failed for %s\n", belle_songfile);
        free(songptr);
        songptr = NULL;
        songlen = 0;
        MUSIC_ErrorCode = MUSIC_MidiError;
        return MUSIC_Error;
    }

    MUSIC_ErrorCode = MUSIC_Ok;
    return MUSIC_Ok;
}

int MUSIC_StopSong(void)
{
    stopsong();
    MUSIC_ErrorCode = MUSIC_Ok;
    return MUSIC_Ok;
}

// --- The rest of music.h: no-op stubs (unused by jftekwar, kept for a clean
// --- link and for future milestones) ---

int   MUSIC_GetCurrentDriver(void) { return ASS_AutoDetect; }
const char *MUSIC_GetCurrentDriverName(void) { return "belle_music (OGG)"; }
void  MUSIC_SetMaxFMMidiChannel(int c) { (void)c; }
void  MUSIC_SetMidiChannelVolume(int c, int v) { (void)c; (void)v; }
void  MUSIC_ResetMidiChannelVolumes(void) {}
int   MUSIC_GetVolume(void) { return musicvolume; }
void  MUSIC_SetLoopFlag(int l) { (void)l; }
int   MUSIC_SongPlaying(void) { return songvoice >= 0 && FX_SoundActive(songvoice); }
void  MUSIC_Continue(void) {}
void  MUSIC_Pause(void) {}
void  MUSIC_SetContext(int c) { (void)c; }
int   MUSIC_GetContext(void) { return 0; }
void  MUSIC_SetSongTick(unsigned int t) { (void)t; }
void  MUSIC_SetSongTime(unsigned int t) { (void)t; }
void  MUSIC_SetSongPosition(int m, int b, int t) { (void)m; (void)b; (void)t; }
void  MUSIC_GetSongPosition(songposition *p) { (void)p; }
void  MUSIC_GetSongLength(songposition *p) { (void)p; }
int   MUSIC_FadeVolume(int tov, int ms) { (void)tov; (void)ms; return MUSIC_Ok; }
int   MUSIC_FadeActive(void) { return 0; }
void  MUSIC_StopFade(void) {}
void  MUSIC_RerouteMidiChannel(int c, int (*fn)(int, int, int)) { (void)c; (void)fn; }
void  MUSIC_RegisterTimbreBank(unsigned char *t) { (void)t; }

#endif /* __SYMBIAN32__ */
