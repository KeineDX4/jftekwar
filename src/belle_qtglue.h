// belle_qtglue.h -- interface between the C platform layer (belle_layer.c) and
// the Qt shell (belle_main.cpp) for the JFTekWar -> Symbian Belle port.
//
// Threading model:
//   - the game busy-loop runs on a QThread (GameWorker); it calls handleevents()
//     each frame, which drains the SPSC input queue (belle_layer.c);
//   - the Qt main thread owns the QApplication + GameWidget; it produces key
//     events via belle_push() and consumes the RGB32 frame via belle_frame_*;
//   - the frame buffer is protected by a QMutex owned by belle_main.cpp
//     (belle_frame_lock/unlock), so showframe() (game thread) and paintEvent()
//     (main thread) never race.
//
// Logging: ONE file, tekwar.log, written by the engine's own
// buildsetlogfile("tekwar.log") into the WORKING DIRECTORY -- which the game
// thread chdir()s to BELLE_GAME_DIR (the game-data folder from belle_config.h,
// next to STUFF.DAT). No port diagnostics are mixed in and no debug output
// channel exists at all.

#ifndef __belle_qtglue_h__
#define __belle_qtglue_h__

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic clock sources, installed by belle_main.cpp before the game thread
   starts. belle_layer's inittimer/sampletimer/getticks route through these. */
extern unsigned int (*belle_ticks_ms)(void);   /* milliseconds, never decreases */
extern unsigned int (*belle_ticks_us)(void);   /* microseconds, never decreases */

/* Push one key event into the input queue (main thread -> game thread).
   scan: DOS/PC scancode (0x01 esc, 0x39 space, 0xc8..0xcd arrows, ...); 0 = none.
   press: 1 = key down, 0 = key up.
   ascii: printable character produced by the key (0 if none). */
void belle_push(int scan, int press, int ascii);
/* v29: auto-repeat push for a HELD key. Re-affirms keystatus only -- never
   keyfifo (so the rebind capture's bgetkey can't re-catch a held key on every
   repeat) and never a character. Lets a repeat press re-latch a key whose
   keystatus a repeat-cycle release cleared. */
void belle_push_repeat(int scan, int press);

/* --- Audio. Called by driver_belle.c (game thread), implemented in
   belle_main.cpp. Each open/start/stop/shutdown is queued and executed by the
   Qt main thread (belle_audio_exec_cmds, driven by the paint tick), which
   forwards the
   actual CMMFDevSound work to a dedicated audio worker thread (BelleAudioWorker)
   -- no MMF call ever runs on the game or main thread. lock/unlock are the
   shared QMutex around the mixer, taken by the game thread (via the driver) and
   by the audio worker's DoBufferToBeFilled, which mixes on demand at DevSound's
   own cadence.
   belle_audio_start carries the mixer buffer contract straight from
   MV_StartPlayback: BufferStart=MV_MixBuffer[0], BufferSize=MV_BufferSize,
   NumDivisions=MV_NumberOfBuffers, CallBackFunc=MV_ServiceVoc. */
int  belle_audio_open(int *rate, int *channels, int *bits);
int  belle_audio_start(char *BufferStart, int BufferSize, int NumDivisions,
                       void (*CallBackFunc)(void));
void belle_audio_stop(void);
void belle_audio_shutdown(void);
void belle_audio_lock(void);
void belle_audio_unlock(void);

/* RGB32 frame buffer written by showframe() (game thread), read by paintEvent()
   (main thread). Width/height/pitch are valid only while belle_frame_lock() is held. */
unsigned char *belle_frame_ptr(void);
int belle_frame_width(void);
int belle_frame_height(void);
int belle_frame_pitch(void);

/* Implemented in belle_main.cpp: QMutex guard for the frame buffer, and a short
   CPU yield the game thread takes when there is no input pending so the Qt
   main thread always gets scheduling room on the single-core E7. */
void belle_frame_lock(void);
void belle_frame_unlock(void);
void belle_qt_yield(void);
void belle_qt_sleep(int ms);   /* v14: pace the game loop (belle_layer.c showframe) */

/* Frame-driven repaint (v13). showframe() (game thread) bumps a monotonic frame
   counter read by belle_frame_seq_value(); the Qt main thread SELF-PACES its
   repaints (paintEvent schedules the next one via QTimer::singleShot) and skips
   paints whose counter is unchanged. */
unsigned int belle_frame_seq_value(void);

#ifdef __cplusplus
}
#endif

#endif /* __belle_qtglue_h__ */
