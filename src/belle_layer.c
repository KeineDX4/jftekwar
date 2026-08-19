// belle_layer.c -- real Qt baselayer for the JFTekWar -> Symbian Belle port.
//
// This is the platform layer the game links against (see baselayer.h for the
// full contract). It replaces the earlier no-op stublayer.c (removed): it
// presents a real frame
// surface (8-bit frameplace + engine palette -> RGB32 buffer), feeds keyboard
// input into keystatus/keyfifo/keyasciififo, and drives totalclock from a
// monotonic clock.
//
// Threading: the game busy-loop runs on a QThread (see belle_main.cpp). This
// file is the "game side" of the interface declared in belle_qtglue.h:
//   - belle_push() is called from the Qt main thread and only appends to an
//     SPSC ring; handleevents() drains it on the game thread.
//   - showframe() writes the RGB32 frame under belle_frame_lock() (a QMutex in
//     belle_main.cpp); the widget's paintEvent reads it under the same lock.
//
// Symbol ownership (link-time): engine/src/baselayer.c already defines all the
// globals in baselayer.h (xres/yres/bpp/fullscreen/bytesperline/imageSize,
// frameplace, keystatus/keyfifo/keyasciififo, quitevent, inputdevices, ...).
// Only `displaycnt` is left for the platform layer. Nothing here redefines a
// symbol baselayer.c provides -- the filename (not baselayer.c) also avoids a
// flat-object-name collision with it in the sbs build.

#include "compat.h"       // PRINTF_FORMAT, endianness macros baselayer.h needs
#include "baselayer.h"    // platform contract, all engine globals
#include "baselayer_priv.h" // initsystem/uninitsystem
#include "build.h"        // ylookup, numpages, totalclock, palette_t, curpalettefaded
#include "osd.h"          // OSD_CaptureKey/HandleKey/HandleChar/ShowDisplay/ResizeDisplay
#include "a.h"            // setvlinebpl
#include "belle_qtglue.h" // C<->C++ interface with belle_main.cpp

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// --- globals ---
int displaycnt;                                 // only platform global baselayer.c lacks

// Clock sources installed by belle_main.cpp before the game thread starts.
unsigned int (*belle_ticks_ms)(void) = NULL;
unsigned int (*belle_ticks_us)(void) = NULL;

volatile unsigned int belle_frame_seq = 0;   // bumped by showframe() per frame

// --- timer ---
static int timerticspersec;
static unsigned int timerlastsample;            // (ms * tps / 1000) at last sample
static void (*usertimercallback)(void);

// --- frame buffer (written under belle_frame_lock) ---
static unsigned char *belle_rgb32;
static int belle_rgb32_w, belle_rgb32_h, belle_rgb32_pitch;

// --- SPSC input queue (single producer: Qt main thread; single consumer: game thread) ---
#define BELLE_QUEUE_SIZ 64
static struct { int scan; unsigned char press, ascii; unsigned char repeat; } belle_q[BELLE_QUEUE_SIZ];
static volatile int belle_qhead, belle_qtail;

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------

// Mode list so the game's default request (640x480x8 fullscreen) gets an exact
// match in checkvideomode(). Without any mode, Set_GameMode exits the game.
void getvalidmodes(void)
{
    if (validmodecnt) return;

    addvalidmode(640, 480, 8, 1, 0, 0, -1);     // default request: fullscreen 4:3
    addvalidmode(640, 480, 8, 0, 0, 0, -1);     // windowed fallback
    addvalidmode(640, 400, 8, 1, 0, 0, -1);
    addvalidmode(640, 400, 8, 0, 0, 0, -1);
    addvalidmode(320, 200, 8, 1, 0, 0, -1);
    addvalidmode(320, 200, 8, 0, 0, 0, -1);
    addvalidmode(640, 360, 8, 1, 0, 0, -1);     // native 16:9
    addvalidmode(640, 360, 8, 0, 0, 0, -1);

    sortvalidmodes();
}

// Establish the 8-bit software frame surface exactly as sdlayer2.c does. The
// engine then adopts xres/yres as the real mode.
int setvideomode(int xdim, int ydim, int bitspp, int fullsc)
{
    int i, j;
    int display = (fullsc & 255) ? (fullsc >> 8) : 0;

    (void)display;      // single display on the E7
    fullsc &= 255;

    if (bitspp != 8) return -1;   // only the 8-bit software renderer for now

    bytesperline = (((xdim | 1) + 4) & ~3);

    if (frameplace) free((void *)(intptr_t)frameplace);
    frameplace = (intptr_t)malloc(bytesperline * ydim);
    if (!frameplace) { frameplace = 0; return -1; }

    // RGB32 output buffer: swap under the frame lock so paintEvent never sees
    // a torn pointer/size tuple.
    belle_frame_lock();
    if (belle_rgb32) free(belle_rgb32);
    belle_rgb32 = (unsigned char *)malloc(xdim * ydim * 4);
    if (!belle_rgb32) {
        belle_frame_unlock();
        free((void *)(intptr_t)frameplace);
        frameplace = 0;
        return -1;
    }
    belle_rgb32_w = xdim;
    belle_rgb32_h = ydim;
    belle_rgb32_pitch = xdim * 4;
    belle_frame_unlock();

    imageSize = bytesperline * ydim;
    setvlinebpl(bytesperline);
    for (i = j = 0; i <= ydim; i++) { ylookup[i] = j; j += bytesperline; }
    numpages = 1;

    xres = xdim;
    yres = ydim;
    bpp = bitspp;
    fullscreen = fullsc;
    videomodereset = 0;

    if (baselayer_videomodedidchange) baselayer_videomodedidchange();
    OSD_ResizeDisplay(xres, yres);

    debugprintf("setvideomode %dx%d bpp=%d full=%d\n", xdim, ydim, bitspp, fullsc);
    return 0;
}

// Convert the 8-bit frameplace using the engine's current faded palette into
// the RGB32 buffer the Qt widget blits (Format_RGB32, little-endian 0xAABBGGRR).
void showframe(void)
{
    int y, x;

    if (!frameplace || !belle_rgb32) return;

    belle_frame_lock();
    {
        const unsigned char *src = (const unsigned char *)(intptr_t)frameplace;
        unsigned int *dst = (unsigned int *)belle_rgb32;
        const int stride = belle_rgb32_pitch / 4;
        const palette_t *pal = curpalettefaded;

        for (y = 0; y < yres; y++) {
            unsigned int *row = dst + y * stride;
            for (x = 0; x < xres; x++) {
                const unsigned char idx = src[x];
                row[x] = 0xff000000u
                       | ((unsigned)pal[idx].r << 16)
                       | ((unsigned)pal[idx].g << 8)
                       | (unsigned)pal[idx].b;
            }
            src += bytesperline;
        }
    }
    belle_frame_unlock();

    // v13: no cross-thread update post here (removed). The Qt main thread now
    // self-paces its repaints (QTimer::singleShot in paintEvent) and samples the
    // newest frame via belle_frame_seq -- posting a queued update per frame made
    // the game thread block while the GUI thread painted (v11/v12 mutual
    // throttling to ~10fps). The frame counter alone drives the paint.
    belle_frame_seq++;

    // v14 perf: pace the game loop at ~30 fps. The 576x360 drawImage blit is
    // cheap uncontended (~9ms -- see the BLITDI lines right after each HB, when
    // the game thread is stalled on the sw.debug file write) but ~50ms when the
    // game thread hogs the single core (draw ~18ms/frame, and handleevents only
    // yields when NO key is held, so active play starves the Qt main thread
    // mid-blit). Sleeping here -- at the one per-frame showframe call (engine.c
    // qsetmode==200) -- leaves the core free for paintEvent between game frames,
    // cutting the contended blit back to its uncontended cost. Self-correcting:
    // a frame that overruns its 33ms slot just slips the deadline forward.
    {
        static unsigned int pace_deadline = 0;
        unsigned int now = belle_ticks_ms ? belle_ticks_ms() : 0;
        if (now < pace_deadline) {
            unsigned int wait = pace_deadline - now;
            if (wait > 40) wait = 40;           // cap: never sleep > ~1.3 frames
            belle_qt_sleep((int)wait);
        }
        pace_deadline = now + 33;               // 33 ms == ~30 fps
    }
}

unsigned int belle_frame_seq_value(void) { return belle_frame_seq; }

int setpalette(int start, int num, unsigned char *dapal)
{
    // The engine keeps its own curpalettefaded (see setbrightness); nothing to
    // push to a device palette in an RGB blit pipeline.
    (void)start; (void)num; (void)dapal;
    return 0;
}

int setsysgamma(float shadergamma, float sysgamma)
{
    // Always report failure: this build has no system-level gamma control
    // (RGB32 blit, engine-side palette). Returning < 0 keeps the engine on its
    // palette path (usegammabrightness -> 0), so setbrightness() applies the
    // brightness table to curpalettefaded -- the values showframe() actually
    // renders. Returning 0 would make the engine believe the gamma was applied
    // system-side and skip the palette correction, leaving the in-game
    // brightness slider a no-op.
    (void)shadergamma; (void)sysgamma;
    return -1;
}

const char *getdisplayname(int display)
{
    (void)display;
    return "";
}

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------

int inittimer(int tickspersecond, void (*callback)(void))
{
    timerticspersec = tickspersecond;
    usertimercallback = callback;
    timerlastsample = (unsigned)((unsigned long long)
        (belle_ticks_ms ? belle_ticks_ms() : 0) * tickspersecond / 1000);
    return 0;
}

void uninittimer(void)
{
}

void sampletimer(void)
{
    unsigned now;
    int n;

    if (!timerticspersec || !belle_ticks_ms) return;

    now = (unsigned)((unsigned long long)belle_ticks_ms() * timerticspersec / 1000);
    n = (int)(now - timerlastsample);       // wraps with timerlastsample: still correct
    if (n > 0) {
        totalclock += n;
        timerlastsample += (unsigned)n;
        if (usertimercallback) while (n--) usertimercallback();
    }
}

unsigned int getticks(void)
{
    return belle_ticks_ms ? belle_ticks_ms() : 0;
}

unsigned int getusecticks(void)
{
    return belle_ticks_us ? belle_ticks_us() : 0;
}

int gettimerfreq(void)
{
    return 1000000;                         // the belle_ticks_us scale
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// Main-thread producer. Single-writer, single-reader ring; the tail index is
// updated only after the entry is fully written, so a preemption between the
// writes cannot expose a torn entry to the consumer on the single-core E7.
void belle_push(int scan, int press, int ascii)
{
    int nxt = (belle_qtail + 1) & (BELLE_QUEUE_SIZ - 1);
    if (nxt == belle_qhead) return;         // queue full: drop the key

    belle_q[belle_qtail].scan = scan;
    belle_q[belle_qtail].press = (unsigned char)press;
    belle_q[belle_qtail].ascii = (unsigned char)ascii;
    belle_q[belle_qtail].repeat = 0;
    belle_qtail = nxt;
}

// v29: auto-repeat push for a held key -- re-affirms keystatus only (handled in
// handleevents), never keyfifo. A held key's repeats must not re-enter the
// rebind capture, and must be able to re-latch keystatus after a repeat-cycle
// release that slipped past the keyReleaseEvent auto-repeat filter.
void belle_push_repeat(int scan, int press)
{
    int nxt = (belle_qtail + 1) & (BELLE_QUEUE_SIZ - 1);
    if (nxt == belle_qhead) return;         // queue full: drop

    belle_q[belle_qtail].scan = scan;
    belle_q[belle_qtail].press = (unsigned char)press;
    belle_q[belle_qtail].ascii = 0;
    belle_q[belle_qtail].repeat = 1;
    belle_qtail = nxt;
}

// Game-thread consumer. Mirrors sdlayer2.c handleevents() key handling exactly
// (OSD_CaptureKey/OSD_HandleKey/OSD_HandleChar interposition, then
// keystatus + keyfifo/keyasciififo). MUST call sampletimer() each frame -- that
// is what advances totalclock and unblocks the boot path (LogoLevel waits on
// totalclock > 5*120).
int handleevents(void)
{
    int drained = 0;

    while (belle_qhead != belle_qtail) {
        int scan = belle_q[belle_qhead].scan;
        int press = belle_q[belle_qhead].press;
        int ascii = belle_q[belle_qhead].ascii;
        int repeat = belle_q[belle_qhead].repeat;
        belle_qhead = (belle_qhead + 1) & (BELLE_QUEUE_SIZ - 1);
        drained = 1;

        // v29: auto-repeat press -- re-affirm keystatus only, never keyfifo
        // (keyfifo feeds the rebind capture's bgetkey; a repeat must not be
        // taken as a fresh keypress) and never a character. A repeat RELEASE is
        // ignored: it is part of the phone's repeat cycle, not a real key-up.
        if (repeat) {
            if (press) keystatus[scan] = 1;
            continue;
        }

        // Character text (sdlayer2 SDL_TEXTINPUT path). The character produced
        // by the OSD toggle key itself is suppressed, like sdlayer2's
        // eattextinput flag.
        if (ascii && !(scan == OSD_CaptureKey(-1) && press)) {
            if (OSD_HandleChar(ascii)) {
                if (((keyasciififoend + 1) & (KEYFIFOSIZ - 1)) != keyasciififoplc) {
                    keyasciififo[keyasciififoend] = (unsigned char)ascii;
                    keyasciififoend = (keyasciififoend + 1) & (KEYFIFOSIZ - 1);
                }
            }
        }

        if (!scan) continue;                // modifier / unmapped key: nothing else

        // OSD capture: the console's toggle key shows the console and is not
        // delivered to the game.
        if (scan == OSD_CaptureKey(-1)) {
            if (press) OSD_ShowDisplay(-1);
        } else if (OSD_HandleKey(scan, press) == 0) {
            // consumed by the open console
        } else if (press) {
            // First press sets keystatus (auto-repeat presses find it already 1,
            // mirroring sdlayer2's "!repeat" guard); every press pushes keyfifo.
            if (!keystatus[scan]) keystatus[scan] = 1;
            keyfifo[keyfifoend] = scan;
            keyfifo[(keyfifoend + 1) & (KEYFIFOSIZ - 1)] = 1;
            keyfifoend = (keyfifoend + 2) & (KEYFIFOSIZ - 1);
        } else {
            keystatus[scan] = 0;
            keyfifo[keyfifoend] = scan;
            keyfifo[(keyfifoend + 1) & (KEYFIFOSIZ - 1)] = 0;
            keyfifoend = (keyfifoend + 2) & (KEYFIFOSIZ - 1);
        }
    }

    sampletimer();

    // Give the Qt main thread CPU when nothing happened this frame; the game
    // busy-loop would otherwise starve it on the single-core E7.
    if (!drained)
        belle_qt_yield();

    return 0;
}

int initinput(void) { return 0; }
void uninitinput(void) {}
void releaseallbuttons(void) {}
// getkeyname: JFTekWar has no key-rebind menu and the engine never calls it
// (the platform contract only requires the symbol -- sdlayer2.c/winlayer.c
// define it for their own platforms). Return "" like the earlier stub.
const char *getkeyname(int num) { (void)num; return ""; }
const char *getjoyname(int what, int num) { (void)what; (void)num; return ""; }

int initmouse(void) { return 0; }
void uninitmouse(void) {}
void grabmouse(int a) { (void)a; }
void readmousexy(int *x, int *y) { *x = 0; *y = 0; }
void readmousebstatus(int *b) { *b = 0; }

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void initputs(const char *str) { (void)str; }

// Diagnostic trace. The game routes MONO_PRINT (game.h) here -- DispMono is
// TRUE -- and the engine routes fatal _Assert/_ErrMsg here too. No-op: the
// engine's own tekwar.log is the only log channel.
void debugprintf(const char *f, ...)
{
    (void)f;
}

// ---------------------------------------------------------------------------
// Window / dialog management (no GUI in the game thread)
// ---------------------------------------------------------------------------

// No GUI dialogs live on the game thread, so wm_msgbox routes the message into
// the engine's tekwar.log channel (buildprintf -> buildputs -> the logfile) instead
// of showing a box. jfmact/util_lib.c Error() calls this right before exit(),
// so the fatal message at least lands on record.
int wm_msgbox(const char *name, const char *fmt, ...)
{
    char buf[1024];
    va_list va;

    va_start(va, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);

    if (name && name[0])
        buildprintf("[%s] %s\n", name, buf);
    else
        buildprintf("%s\n", buf);
    return 0;
}

void wm_setapptitle(const char *name) { (void)name; }
void wm_setwindowtitle(const char *name) { (void)name; }

// ---------------------------------------------------------------------------
// Frame-buffer accessors for the Qt widget
// ---------------------------------------------------------------------------

unsigned char *belle_frame_ptr(void) { return belle_rgb32; }
int belle_frame_width(void) { return belle_rgb32_w; }
int belle_frame_height(void) { return belle_rgb32_h; }
int belle_frame_pitch(void) { return belle_rgb32_pitch; }

// ---------------------------------------------------------------------------
// initsystem/uninitsystem (baselayer_priv.h), called from initengine()/
// uninitengine(). The real device setup happens in the Qt main thread.
// ---------------------------------------------------------------------------

int initsystem(void) { return 0; }
void uninitsystem(void) {}

// ---------------------------------------------------------------------------
// fstatat fallback
// Symbian's libc provides dirfd() but not fstatat(). Both call sites pass a
// bare filename (an entry from readdir of the already-open directory) and
// flags==0, so we fall back to a CWD-relative stat(). On failure the callers
// leave their stat fields zeroed, which is graceful.
// ---------------------------------------------------------------------------

int fstatat(int dirfd, const char *path, struct stat *buf, int flags)
{
    (void)dirfd; (void)flags;
    return stat(path, buf);
}
