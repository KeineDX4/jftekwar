// belle_music.h -- JFTekWar -> Symbian Belle port: music.h shim interface.
// belle_music.c implements the MUSIC_* API by playing the OGG pre-render named
// in belle_songfile (set by teksnd.c startmusic()/menusong()) through the
// FX/vorbis path. Declared separately so teksnd.c can set the file name; only
// visible to the Symbian build.
#ifndef __belle_music_h__
#define __belle_music_h__
#ifdef __SYMBIAN32__
extern char belle_songfile[BMAX_PATH+1];
#endif
#endif /* __belle_music_h__ */
