// belle_main.cpp -- Qt 4.7.4 shell for the JFTekWar -> Symbian Belle port.
//
// The game's busy-loop (app_main, which boots to the mission menu and the game
// loop) runs on a QThread; the Qt main thread owns the
// fullscreen widget and the event loop. Per frame the widget blits the newest
// RGB32 frame (written by showframe() in belle_layer.c) scaled with aspect-fit,
// and forwards hardware-key events into the platform layer via belle_push().
//
// Repaints are SELF-PACED (v13): paintEvent schedules the next repaint 15ms out
// (QTimer::singleShot) and samples the newest frame via the game thread's frame
// sequence number, skipping paints whose sequence is unchanged. Earlier schemes
// were both wrong on the single-core E7: (v10) a continuous "update() at the end"
// chain busy-painted and starved the IdlePriority game thread; (v11/v12) a
// per-frame queued update posted from showframe() made the game thread block
// while the GUI thread painted -- mutual throttling to ~10fps. v13 has the game
// thread write frames and never post anything; the GUI thread idles between
// paints so system events stay serviced.
//
// Key->scancode mapping uses the E7 hardware-key values verified on-device (the
// standard Qt 4.7 values). The game reads keystatus[] (gameplay/menus),
// keyfifo[] (key rebinding) and keyasciififo[] (text entry) -- see
// belle_layer.c handleevents().

#include "belle_qtglue.h" // C interface into belle_layer.c
#include "belle_config.h" // Symbian Belle: BELLE_GAME_DIR (game-data folder on the phone)
#include "vorbis_ring.h" // v48: background OGG decoder thread + PCM ring

// The game's entry point (game.c). C linkage as declared in baselayer.h. The
// shell needs nothing else from the engine headers, so we declare it directly
// instead of including baselayer.h -- that would drag in compat.h, a C99
// header which #errors under GCCE C++ (GCCE defines __cplusplus < 199711L).
extern "C" int app_main(int argc, char const * const argv[]);

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QTimer>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QCoreApplication>
#include <QEvent>
#include <QImage>

#ifdef Q_OS_SYMBIAN
#include <e32base.h>    // CBase
#include <mmf/server/sounddevice.h>   // v41: CMMFDevSound + MDevSoundObserver + TMMFCapabilities
#include <mmf/server/mmfdatabuffer.h> // v41: CMMFDataBuffer (the buffers DevSound hands us)
#include <mmf/common/mmfbase.h>       // v41: TMMFState / EMMFStatePlaying
#include <mmf/common/mmffourcc.h>     // KMMFFourCCCodePCM16 (explicit DevSound type)
#endif

#include <cstdlib>   // v48: malloc/free for the vorbis ring slots
#include <cstring>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// Monotonic clock (installed into belle_layer before the game thread starts).
// QElapsedTimer::elapsed() is milliseconds in Qt 4.7 (no nsecsElapsed).
// ---------------------------------------------------------------------------
static QElapsedTimer g_clock;
static unsigned qtMs() { return (unsigned)g_clock.elapsed(); }
static unsigned qtUs() { return (unsigned)(g_clock.elapsed() * 1000); }

// ---------------------------------------------------------------------------
// Frame-buffer guard + idle yield (belle_layer.c calls these from the game thread)
// ---------------------------------------------------------------------------
static QMutex g_frameMutex;

// Main-thread-owned RGB32 screen buffer (640x360), pre-allocated in main()
// BEFORE the game thread starts: the first paint after the game sets its video
// mode otherwise races the game thread's level-load allocations, and on a
// memory-tight E7 that ~700KB allocation is where the main thread can stall or
// die (v7: black screen, MP lines stop at the first real frame). Scaled from
// the shared buffer under the frame lock (a bounded loop), then the used
// sub-rectangle is blitted 1:1 WITHOUT the lock. Never re-allocated.
static QImage g_scaled;
static int g_sw = 0, g_sh = 0;

void belle_frame_lock() { g_frameMutex.lock(); }
void belle_frame_unlock() { g_frameMutex.unlock(); }

// belle_qt_yield() is defined after GameWorker -- it needs GameWorker::pauseMs.

// ---------------------------------------------------------------------------
// Qt::Key -> DOS/PC scancode. E7 hardware values are the standard Qt 4.7 ones
// (verified on-device): 0x01000012/13/14/15 = Left/Up/Right/Down,
// 0x20 = Space, 0x01000000 = Escape, 0x01000004 = Return, 0x01000055 = Menu
// (hardware key under the screen), 0x01000061 = Back, Fn+digit = 0x30..0x39.
// ---------------------------------------------------------------------------
// PC set-1 scancode for a printable US-layout ASCII character (letters in both
// cases, digits, and the shifted/unshifted punctuation pairs). Layout- and
// layer-independent: whatever character code the phone sends for a key, this
// finds its physical key position on a US keyboard.
static int asciiScan(int c)
{
    static const unsigned char az[26] =
        { 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,
          0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14,
          0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c };
    if (c >= 'A' && c <= 'Z') return az[c - 'A'];
    if (c >= 'a' && c <= 'z') return az[c - 'a'];
    switch (c) {
    case ' ':            return 0x39;
    case '1': case '!':  return 0x02;
    case '2': case '@':  return 0x03;
    case '3': case '#':  return 0x04;
    case '4': case '$':  return 0x05;
    case '5': case '%':  return 0x06;
    case '6': case '^':  return 0x07;
    case '7': case '&':  return 0x08;
    case '8': case '*':  return 0x09;
    case '9': case '(':  return 0x0a;
    case '0': case ')':  return 0x0b;
    case '-': case '_':  return 0x0c;
    case '=': case '+':  return 0x0d;
    case '[': case '{':  return 0x1a;
    case ']': case '}':  return 0x1b;
    case '\\': case '|': return 0x2b;
    case ';': case ':':  return 0x27;
    case '\'': case '"': return 0x28;
    case '`': case '~':  return 0x29;
    case ',': case '<':  return 0x33;
    case '.': case '>':  return 0x34;
    case '/': case '?':  return 0x35;
    default:             return 0;
    }
}

// Russian (ЙЦУКЕН) -> US position: translate a committed Cyrillic letter to the
// physical key's DOS scancode, for an E7 whose input language is Russian so its
// text layer commits Cyrillic instead of ASCII letters. Code points used (not
// string literals) so the source stays encoding-independent.
static int cyrillicScan(int c)
{
    if (c >= 0x410 && c <= 0x42f) c += 0x20;          // A-Z -> a-z
    if (c == 0x401) c = 0x451;                        // Ё -> ё
    switch (c) {
    case 0x439: return 0x10;   /* й Q */ case 0x446: return 0x11;   /* ц W */
    case 0x443: return 0x12;   /* у E */ case 0x43a: return 0x13;   /* к R */
    case 0x435: return 0x14;   /* е T */ case 0x43d: return 0x15;   /* н Y */
    case 0x433: return 0x16;   /* г U */ case 0x448: return 0x17;   /* ш I */
    case 0x449: return 0x18;   /* щ O */ case 0x437: return 0x19;   /* з P */
    case 0x445: return 0x2d;   /* х X */ case 0x44a: return 0x1a;   /* ъ [ */
    case 0x444: return 0x1e;   /* ф A */ case 0x44b: return 0x1f;   /* ы S */
    case 0x432: return 0x20;   /* в D */ case 0x430: return 0x21;   /* а F */
    case 0x43f: return 0x22;   /* п G */ case 0x440: return 0x23;   /* р H */
    case 0x43e: return 0x24;   /* о J */ case 0x43b: return 0x25;   /* л K */
    case 0x434: return 0x26;   /* д L */ case 0x436: return 0x27;   /* ж ; */
    case 0x44d: return 0x28;   /* э ' */ case 0x44f: return 0x2c;   /* я Z */
    case 0x447: return 0x2e;   /* ч C */ case 0x441: return 0x30;   /* с V */
    case 0x43c: return 0x31;   /* м B */ case 0x438: return 0x32;   /* и N */
    case 0x442: return 0x33;   /* т M */ case 0x44c: return 0x34;   /* ь , */
    case 0x431: return 0x35;   /* б . */ case 0x44e: return 0x36;   /* ю / */
    case 0x451: return 0x14;   /* ё T */
    default:     return 0;
    }
}

// Symbian TStdScanCode (EStdKey*) equals the IBM PC set-1 scancode, which is
// the very value the engine calls the "DOS scancode" (sc_A=0x1e, sc_Q=0x10,
// sc_1=0x02, ...), so a physical key's native scan code maps 1:1 -- with
// Backspace overridden to Escape to match mapQtKey (the E7 has no Escape key).
static int mapNativeScan(int ns)
{
    if (ns == 0x0e) return 0x01;   // EStdKeyBackspace -> Escape
    return ns;
}

// Resolve one QKeyEvent to a DOS scancode: the Qt key code first, then -- for
// codes the map does not recognize (e.g. Cyrillic from a Russian layout, or
// exotic values from the phone's numeric/symbol layer) -- the PHYSICAL key
// position via nativeScanCode(). Press and release MUST go through the same
// path so a release always clears the keystatus the press set.
static int mapQtKey(int k);   // fwd decl: scanOf precedes its definition below
static int scanOf(QKeyEvent *e)
{
    int scan = mapQtKey(e->key());
    if (scan) return scan;
    return mapNativeScan((int)e->nativeScanCode());
}

static int mapQtKey(int k)
{
    switch (k) {
    case 0x01000000: return 0x01;            // Escape
    case 0x01000004: return 0x1c;            // Return
    case 0x01000005: return 0x1c;            // Key_Enter (numpad code -- Symbian reports the hw Enter as this)
    case 0x01000060: return 0x1c;            // Key_Select (E7 D-pad centre / OK) acts as Enter
    case 0x20:       return 0x39;            // Space
    case 0x01000012: return 0xcb;            // Left
    case 0x01000013: return 0xc8;            // Up
    case 0x01000014: return 0xcd;            // Right
    case 0x01000015: return 0xd0;            // Down
    case 0x01000001: return 0x0f;            // Tab
    case 0x01000003: return 0x01;            // Backspace -> Escape (no Esc key on E7; opens menu / cancels)
    case 0x01000016: return 0xc9;            // PageUp
    case 0x01000017: return 0xd1;            // PageDown
    case 0x01000010: return 0xc7;            // Home
    case 0x01000011: return 0xcf;            // End
    case 0x01000007: return 0xd3;            // Delete
    case 0x01000055: return 0x44;            // Menu (hw key) -> F10 quit menu
    case 0x01000061: return 0x01;            // Back -> Escape (cancel/back)
    case 0x01000021: return 0x1d;            // Control -> LCtrl (classic default Fire)
    case 0x01000020: return 0x2a;            // Shift -> LShift (classic default Run)
    default: break;
    }

    // Any printable US-layout ASCII character -> its PC set-1 scancode. The E7
    // reports QWERTY keys as plain character codes: lowercase letters when
    // unshifted, and in the phone's non-text keyboard layer the top row yields
    // digits and the A-row yields symbols. Mapping the whole printable range
    // keeps EVERY key assignable in the rebind window (the "catch any keycode"
    // ask) instead of gating on "is it a letter".
    if (k >= 0x20 && k <= 0x7e) return asciiScan(k);

    // F1..F10: 0x01000030..0x01000039 -> 0x3b..0x44
    if (k >= 0x01000030 && k <= 0x01000039)
        return 0x3b + (k - 0x01000030);

    return 0;   // unmapped
}

// v26: printable character for a scancode, derived from the POSITIONAL US
// scancode instead of e->text(). The phone's system keyboard layout no longer
// matters -- a Russian layout used to deliver Cyrillic bytes (>= 0x80) which
// keyPressEvent dropped, so text fields (save names, console) received
// nothing; now they always get plain US uppercase letters and digits, i.e. the
// English layout is effectively forced for game text. Letters come out
// uppercase (Qt::Key only has uppercase letter codes), which is exactly what
// the rebind window wants: a lowercase letter registers as its uppercase key.
static int scanAscii(int scan)
{
    if (scan >= 0x02 && scan <= 0x0b)            // sc_1..sc_0
        return "1234567890"[scan - 0x02];
    switch (scan) {
    case 0x1e: return 'A';  case 0x30: return 'B';  case 0x2e: return 'C';
    case 0x20: return 'D';  case 0x12: return 'E';  case 0x21: return 'F';
    case 0x22: return 'G';  case 0x23: return 'H';  case 0x17: return 'I';
    case 0x24: return 'J';  case 0x25: return 'K';  case 0x26: return 'L';
    case 0x32: return 'M';  case 0x31: return 'N';  case 0x18: return 'O';
    case 0x19: return 'P';  case 0x10: return 'Q';  case 0x13: return 'R';
    case 0x1f: return 'S';  case 0x14: return 'T';  case 0x16: return 'U';
    case 0x2f: return 'V';  case 0x11: return 'W';  case 0x2d: return 'X';
    case 0x15: return 'Y';  case 0x2c: return 'Z';
    case 0x39: return ' ';              // Space
    case 0x1c: return '\r';             // Enter
    case 0x0e: return 8;                // BackSpace
    case 0x0f: return 9;                // Tab
    default:   return 0;
    }
}

// Audio forward declarations -- the paint tick below dispatches mixer
// commands; the full definitions live in the audio block just before main().
// Declared here because the GameWidget paintEvent (defined below this point)
// calls belle_audio_exec_cmds(), and member functions defined inside a class
// only see globals declared BEFORE it.
static QMutex g_audioMutex;
static void belle_audio_exec_cmds();
// Ring + stream state shared between the audio worker (which mixes on demand
// inside BufferToBeFilled) and the main thread's diagnostics. Declared before
// GameWidget only for the paint tick's call to belle_audio_exec_cmds() above.
// produced/consumed ring counters (guarded by g_audioMutex) are the only
// cross-thread state the worker touches directly.
static int g_audioProduced = 0;   // blocks MV_ServiceVoc has mixed (audio worker, v44)
static int g_audioConsumed = 0;   // blocks the audio worker has pulled
static int g_audioStarted = 0;    // set when the START command succeeds. Declared HERE
                                  // (not in the exec_cmds block below) because
                                  // BelleMmfOutput's DoBufferToBeFilled (defined
                                  // before that block) gates mixing on it (v44).

class GameWidget : public QWidget
{
public:
    GameWidget()
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAttribute(Qt::WA_NoSystemBackground);
        // v29/v30: enable the input method so the E7 treats this widget as a
        // text target and switches its QWERTY to the LETTER layer. With it
        // disabled the phone keeps the keyboard in its non-text (phone/number)
        // layer and the top row types digits / the A-row types symbols -- the
        // exact "letters can't be assigned" symptom. v29 found the catch: with
        // the IM enabled, letter/Space keys are committed as QInputMethodEvent
        // (text), NOT QKeyEvent, so inputMethodEvent() below converts them back
        // into scan/ascii pairs. The hardware keyboard stays in use (no VKB
        // appears on a phone with the QWERTY open).
        setAttribute(Qt::WA_InputMethodEnabled, true);
        setInputMethodHints(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase
                            | Qt::ImhPreferLowercase);
        setFocusPolicy(Qt::StrongFocus);
        // v31: input-method commit hold. The E7 text layer delivers letter/Space
        // keys as committed text with NO key-up, so we synthesize a release
        // IM_HOLD_MS after the last commit (paintEvent polls it below) -- the
        // rebind capture read the press from keyfifo immediately (works), but
        // gameplay reads keystatus and a press+release drained in the SAME
        // handleevents pass was invisible (0->1->0 before the engine sampled).
        // With the delayed release, keystatus stays down while auto-repeat keeps
        // committing (hold = continuous), and clears ~IM_HOLD_MS after the last
        // commit (tap = one solid press).
        imPendingScan = -1;
        imReleaseAt = 0;
#ifdef Q_OS_SYMBIAN
        setAttribute(Qt::WA_LockLandscapeOrientation);
#endif
    }

protected:
    void paintEvent(QPaintEvent *)
    {
        // v13: SELF-PACED repaint. Schedule the next repaint 15ms after this one
        // finishes, then paint the newest frame if one exists. This replaces both
        // previous schemes: (v10) the continuous "update() at the end" chain busy-
        // painted and starved the game thread; (v11/v12) per-frame queued update
        // posted from showframe() made the game thread block on the post while the
        // GUI thread painted, and the two threads throttled each other on the
        // single core (visible ~10fps despite the game rendering 320x200 in ~20ms).
        // With self-pacing the game thread never posts anything; it just writes
        // frames, and this thread samples the newest one at its own pace, idling
        // between paints so system events are always serviced (v12 process was
        // being killed ~5s into level 2, consistent with a Symbian watchdog for an
        // unresponsive app). The seq gate below skips paints with no new frame.
        QTimer::singleShot(15, this, SLOT(update()));

        // v31: synthesize the release of an input-method-committed key once its
        // hold window elapsed. paintEvent is a ~15ms main-thread tick even when
        // the frame is unchanged (the seq gate below skips only the painting),
        // so this is a safe release pump -- no timers/slots needed on this
        // MOC-less build. Same producer thread (main) as the IM commit pushes.
        if (imPendingScan >= 0 && belle_ticks_ms
            && (int)(belle_ticks_ms() - imReleaseAt) >= 0) {
            belle_push(imPendingScan, 0, 0);
            imPendingScan = -1;
        }

        // Service the audio command queue (open/start/stop/shutdown) at the
        // top of every ~15ms main-thread tick, BEFORE the seq gate below (which
        // returns early when there is no new frame -- audio commands must still
        // be executed even between frames).
        belle_audio_exec_cmds();

        bool locked = false;

        // A paint that has no new frame to show is skipped entirely.
        {
            static unsigned int lastSeq = 0;
            unsigned int seq = belle_frame_seq_value();
            if (seq == lastSeq) return;
            lastSeq = seq;
        }

        try {
            QPainter p(this);
            p.fillRect(rect(), Qt::black);

            // Scale the newest frame into the fixed screen-sized buffer under
            // the frame lock, then blit that sub-rectangle 1:1 OUTSIDE the
            // lock. The buffer is pre-allocated in main() (memory is plentiful
            // then) and is NEVER re-allocated here. The lock hold is one fast
            // nearest-neighbor loop (~1ms) with precomputed row/col maps --
            // a per-pixel integer division was ~17ms/frame and killed the
            // framerate (v8 regression).
            belle_frame_lock();
            locked = true;
            {
                const uchar *buf = belle_frame_ptr();
                const int w = belle_frame_width();
                const int h = belle_frame_height();
                const int pitch = belle_frame_pitch();
                if (buf && w > 0 && h > 0) {
                    // Opaque-black the whole 640x360 buffer first: the borders
                    // the scaled frame does NOT cover must read 0xff000000 (not
                    // 0x00000000 / stale pixels) so the full-image blit below
                    // takes Qt's opaque fast path instead of per-pixel blending.
                    g_scaled.fill(0xff000000);
                    // aspect-fit target; by construction always within 640x360
                    const int dw = rect().width(), dh = rect().height();
                    const qreal scale = qMin((qreal)dw / w, (qreal)dh / h);
                    const int sw = qRound(w * scale), sh = qRound(h * scale);
                    if (sw > 0 && sh > 0 && sw <= 640 && sh <= 360) {
                        // nearest-neighbor scale; source is Format_RGB32 (pitch B/row)
                        int rowmap[360], colmap[640];
                        for (int y = 0; y < sh; y++) rowmap[y] = (y * h / sh);
                        for (int x = 0; x < sw; x++) colmap[x] = (x * w / sw);
                        const unsigned int *src = (const unsigned int *)buf;
                        const int spitch = pitch / 4;
                        const int dspitch = g_scaled.bytesPerLine() / 4;
                        unsigned int *dst = (unsigned int *)g_scaled.bits();
                        for (int y = 0; y < sh; y++) {
                            const unsigned int *srow = src + rowmap[y] * spitch;
                            unsigned int *drow = dst + y * dspitch;
                            for (int x = 0; x < sw; x++)
                                drow[x] = srow[colmap[x]];
                        }
                        g_sw = sw;
                        g_sh = sh;
                    }
                }
            }
            belle_frame_unlock();
            locked = false;

            if (g_sw > 0 && !g_scaled.isNull()) {
                // v12: back to plain opaque drawImage. v11's QPixmap::fromImage +
                // drawPixmap measured WORSE (50-170ms: up=35-76 + blit=9-104) --
                // the present path on this Qt/Symbian stack is software-bound
                // either way, and the extra QPixmap upload just added cost.
                // g_scaled is pre-filled opaque black (scale block above), so Qt
                // takes the opaque fast path. The offset centers the g_sw x g_sh
                // content; the buffer's black borders fill the rest, and any
                // clip at the widget edge is invisible black-on-black.
                const int ox = (rect().width()  - g_sw) / 2;
                const int oy = (rect().height() - g_sh) / 2;
                p.drawImage(ox, oy, g_scaled);
            }

        } catch (...) {
            // A failed paint (e.g., an allocation inside Qt's screen blit) must
            // never take down the main thread. Without this, the exception
            // propagates out of the event loop, main() returns, and the process
            // starts shutting down while the game thread is still running --
            // the "freeze, then crash ~1s later" signature of v5/v6/v8.
            if (locked) belle_frame_unlock();
            // No update() retry here -- the next showframe() (game thread)
            // drives the following repaint.
        }
    }

    void inputMethodEvent(QInputMethodEvent *e)
    {
        // v30/v31: with the input method enabled, the E7's TEXT layer commits
        // letter/Space keys as QInputMethodEvent (committed text) instead of
        // QKeyEvent -- that is why v29 went dead on them (keyPressEvent never
        // fired). The rebind capture reads keyfifo, so a plain press sufficed
        // there (v30 worked in the menu); gameplay reads keystatus, so a
        // press+release drained in one handleevents pass was invisible (v30's
        // in-game failure). v31: push the press now, and let paintEvent release
        // it IM_HOLD_MS after the LAST commit for this scan -- auto-repeat keeps
        // committing while the key is physically held (continuous keystatus),
        // and a different scan's hold is released first so fast typing never
        // leaves a stuck key. Special keys (arrows/Enter/Back/Menu) still arrive
        // as QKeyEvent and go through keyPressEvent/keyReleaseEvent.
        const QString s = e->commitString();
        if (!s.isEmpty()) {
            for (int i = 0; i < s.length(); i++) {
                int c = (unsigned short)s.at(i).unicode();
                int scan = asciiScan(c);
                if (!scan && c >= 0x80) scan = cyrillicScan(c);
                if (scan) {
                    if (imPendingScan >= 0 && imPendingScan != scan)
                        belle_push(imPendingScan, 0, 0);   // release the other key
                    belle_push(scan, 1, (c < 0x80) ? c : 0);
                    imPendingScan = scan;
                    imReleaseAt = (belle_ticks_ms ? belle_ticks_ms() : 0)
                                  + IM_HOLD_MS;
                }
            }
        }
        QWidget::inputMethodEvent(e);
    }

    void keyPressEvent(QKeyEvent *e)
    {
        // v26/v29: handle Qt auto-repeat. The engine latches the key in
        // keystatus[] on the first press; a held key's repeats must NOT flood
        // keyfifo (the rebind capture's bgetkey would re-catch the same key and
        // could assign a held Enter to the next menu function). v29: instead of
        // dropping repeats entirely, re-affirm keystatus only, so a repeat-cycle
        // release that slips past the keyReleaseEvent filter cannot leave the
        // key dead.
        if (e->isAutoRepeat()) {
            int rscan = scanOf(e);
            if (rscan) belle_push_repeat(rscan, 1);
            QWidget::keyPressEvent(e);
            return;
        }
        const int scan = scanOf(e);
        if (scan) {
            int ascii = scanAscii(scan);
            if (e->key() == 0x01000003) ascii = 8;  // E7 Backspace = Escape, but keep backspace char for text
            belle_push(scan, 1, ascii);
        }
        QWidget::keyPressEvent(e);
    }

    void keyReleaseEvent(QKeyEvent *e)
    {
        // v29: an auto-repeat RELEASE is part of the phone's key-repeat cycle,
        // NOT a real key-up. Passing it to the engine cleared keystatus while
        // the key was still physically held -- the "held key auto-releases after
        // a moment" bug (the dropped repeat presses never re-latched it).
        if (e->isAutoRepeat()) {
            QWidget::keyReleaseEvent(e);
            return;
        }
        const int scan = scanOf(e);
        if (scan) belle_push(scan, 0, 0);
        QWidget::keyReleaseEvent(e);
    }

private:
    enum { IM_HOLD_MS = 250 };   // v31: how long an IM-committed key stays down
    int imPendingScan;           // last IM-committed scan awaiting its release
    unsigned int imReleaseAt;    // monotonic ms (belle_ticks_ms) to release it
};

class GameWorker : public QThread
{
public:
    GameWorker()
    {
        // 1 MB (v16; back from 2 MB). v15's setStackSize(0x200000) made the
        // worker thread FAIL TO START on this Symbian/Qt: on-device it gave a
        // white screen with NO thread markers, NO heartbeat, NO tekwar.log --
        // app_main never even began. 1 MB is proven (v13/v14 started fine).
        setStackSize(0x100000);
        // v14: IdlePriority (again, now safe). v10's IdlePriority failed only
        // because the CONTINUOUS update()-at-end paint chain kept the Qt main
        // thread permanently ready, so the game thread never got scheduled. The
        // v13 self-paced repaint (singleShot(15) + seq gate) leaves the main
        // thread genuinely idle between paints, so IdlePriority now means the
        // main thread PREEMPTS the game mid-blit: the 576x360 drawImage runs
        // clean at its uncontended ~9ms instead of the ~50ms the v12/v13
        // contended paints measured. The 30fps frame pace in showframe() then
        // caps the game so it never spins beyond the display rate.
        setPriority(QThread::IdlePriority);
    }

    // Qt 4.7 has QThread::msleep() protected; this is the public wrapper the
    // platform layer's idle yield calls (belle_qt_yield below).
    static void pauseMs(unsigned long ms) { msleep(ms); }

protected:
    void run()
    {
        // v-final: no thread-lifecycle markers (only the engine's own tekwar.log
        // is written).
        try {
            char const *argv[2] = { "JFTEKWAR", NULL };
            app_main(1, argv);
        } catch (...) {
            // v14 safety net: a C++ exception / Symbian leave escaping app_main
            // must not silently kill the process.
        }
    }
};

// Idle yield called from belle_layer.c's handleevents() when no input was
// pending, so the Qt main thread always gets scheduling room on the E7.
void belle_qt_yield() { GameWorker::pauseMs(2); }

// Frame pace (v14): called from belle_layer.c's showframe() to cap the game loop
// at ~30 fps so paintEvent gets clean single-core CPU windows.
void belle_qt_sleep(int ms) { if (ms > 0) GameWorker::pauseMs((unsigned long)ms); }

// ---------------------------------------------------------------------------
// Audio v41: native Symbian MMF output via CMMFDevSound DIRECT.
//
// DevSound drives itself: InitializeL(pcm16, playing) -> SetConfigL -> PlayInitL,
// then pulls buffers through BufferToBeFilled (fill + PlayData, self-sustaining).
//
// Threading model: the stream lives on the DEDICATED AUDIO WORKER THREAD
// (BelleAudioThread below), not the Qt main thread -- DevSound assumes
// creation and use from a single thread, and any wedge can only block the
// sacrificial worker, never the GUI. The worker receives commands as posted
// QEvents (CmdOpen/CmdRing/CmdStop); DevSound's active objects
// are serviced by the worker's Qt 4.7 event loop (= active scheduler).
//   - the game thread reaches audio ONLY through the belle_audio_* hooks.
//     open/start/stop/shutdown are executed on the Qt main thread at the top of
//     paintEvent; the game thread posts a command and waits on a QWaitCondition.
//   - the mixer (MV_ServiceVoc) runs ON THE AUDIO WORKER, mix-on-demand inside
//     BufferToBeFilled: for each DevSound buffer it mixes exactly the blocks it
//     copies (lockstep), so MV_MixPage and the ring's produced/consumed counters
//     stay in sync. The voices advance even when the game thread is elsewhere --
//     the difficulty-taunt while(FX_SoundActive()) must not hang.
//   - the ring and the produced/consumed counters are guarded by g_audioMutex;
//     the worker is both producer and consumer, the main thread only dispatches
//     commands and reads the counters for diagnostics.
// ---------------------------------------------------------------------------
#ifdef Q_OS_SYMBIAN
class BelleMmfOutput : public CBase, public MDevSoundObserver
{
public:
    BelleMmfOutput()
        : iDevSound(0), iMix(0), iBlock(0), iCount(0),
          iProduced(0), iConsumed(0), iMixCb(0)
    {
    }

    void OpenStream()
    {
        if (iDevSound) return;   // already opening or open
        // v41: CMMFDevSound::NewL + InitializeL(pcm16, playing). The 3-arg
        // InitializeL takes an explicit pcm16 fourCC so DevSound configures for
        // raw PCM before InitializeComplete lets us override rate/channels.
        // Both calls are asynchronous; completion arrives via InitializeComplete
        // on the audio worker thread.
        TRAPD(err,
            iDevSound = CMMFDevSound::NewL();
            iDevSound->InitializeL(*this, KMMFFourCCCodePCM16, EMMFStatePlaying);
        );
        if (err != KErrNone) {
            delete iDevSound;
            iDevSound = 0;
        }
    }

    void Close()
    {
        if (iDevSound) {
            iDevSound->Stop();
            delete iDevSound;
            iDevSound = 0;
        }
    }

    void SetRing(char *mix, int block, int count, int *produced, int *consumed)
    {
        iMix = mix; iBlock = block; iCount = count;
        iProduced = produced; iConsumed = consumed;
    }

    // v44: the mixer callback (MV_ServiceVoc) now runs HERE, on the audio
    // worker, from DoBufferToBeFilled -- one call per ring block DevSound wants,
    // straight out of the ring (which is now just the mixer's scratch). It is
    // set at START along with the ring (CmdRing), so it always agrees with
    // iMix/iBlock/iCount.
    void SetMixCb(void (*cb)(void)) { iMixCb = cb; }

    // MDevSoundObserver -- every body is TRAPped (v39): a leave in an
    // active-object RunL panics its thread, and even on the sacrificial audio
    // thread a panic takes the whole process down.
    void InitializeComplete(TInt aError)
    {
        TRAPD(ignore, DoInitializeComplete(aError));
        (void)ignore;
    }

    void DoInitializeComplete(TInt aError)
    {
        if (aError != KErrNone) {
            return;
        }
        // Primary = 22050/mono, matching the mixer rate set in AUDIO_CMD_OPEN.
        // 22050 mono needs only 44KB/s and ~86 mixer blocks/s -- headroom both
        // ways on the single-core E7.
        TMMFCapabilities caps = iDevSound->Config();
        caps.iRate = EMMFSampleRate22050Hz;
        caps.iChannels = EMMFMono;
        caps.iEncoding = EMMFSoundEncoding16BitPCM;
        TRAPD(se, iDevSound->SetConfigL(caps));
        if (se != KErrNone) {
            // 22050/mono refused -> 16000/mono, then the E7's native 8000/mono.
            // (The mixer stays at 22050, so a fallback pitch-shifts the FX until
            // the next build -- but it still proves the pipeline end to end.)
            caps.iRate = EMMFSampleRate16000Hz;
            TRAPD(se2, iDevSound->SetConfigL(caps));
            if (se2 != KErrNone) {
                caps.iRate = EMMFSampleRate8000Hz;
                TRAPD(se3, iDevSound->SetConfigL(caps));
                if (se3 != KErrNone) {
                    return;
                }
            }
        }
        iDevSound->SetVolume(iDevSound->MaxVolume());
        TRAPD(pi, iDevSound->PlayInitL());
        if (pi != KErrNone) {
            return;
        }
    }

    void BufferToBeFilled(CMMFBuffer *aBuffer)
    {
        TRAPD(ignore, DoBufferToBeFilled(aBuffer));
        (void)ignore;
    }

    void DoBufferToBeFilled(CMMFBuffer *aBuffer)
    {
        if (!aBuffer) return;
        // DevSound hands us a CMMFDataBuffer (its internal DMA buffer); fill it
        // and return it with PlayData. DevSound then calls BufferToBeFilled
        // again for the next chunk -- a self-sustaining pull.
        CMMFDataBuffer *db = static_cast<CMMFDataBuffer *>(aBuffer);
        TDes8 &data = db->Data();
        const TInt want = data.MaxLength();
        char *dst = (char *)data.Ptr();
        int filled = 0;
        // v44: MIX ON DEMAND. The mixer (MV_ServiceVoc) now runs HERE, on the
        // audio worker, at DevSound's own cadence instead of on the Qt main
        // thread's paint tick. For each 256-sample block DevSound wants we call
        // the mixer -- block #p lands in ring page (2+p)%iCount (MV_MixPage
        // starts at 1 and increments per call) -- and copy that page straight
        // into the buffer. The ring is now just the mixer's scratch: no
        // cross-thread producer, no accumulator cap, no dependence on the main
        // thread. Lockstep holds because we mix a page only to copy it
        // immediately (consumed==produced always), so MV_MixPage and
        // (*iConsumed) advance together.
        g_audioMutex.lock();
        if (iMixCb && iMix && iBlock > 0 && iCount > 0 &&
            iProduced && iConsumed && g_audioStarted) {
            while (filled + iBlock <= want) {
                iMixCb();                          // mixes block #(*iConsumed)
                const int page = (2 + *iConsumed) % iCount;
                memcpy(dst + filled, iMix + page * iBlock, (size_t)iBlock);
                filled += iBlock;
                (*iConsumed)++;
                (*iProduced)++;
            }
        }
        g_audioMutex.unlock();
        // Tail of a short read is silence -- DevSound is always given a full
        // buffer. (For 22050 mono want=4096=8*512, so this is normally empty.)
        if (filled < want) memset(dst + filled, 0, (size_t)(want - filled));
        data.SetLength(want);
        if (iDevSound)
            iDevSound->PlayData();
    }

    void PlayError(TInt aError)
    {
        TRAPD(ignore, DoPlayError(aError));
        (void)ignore;
    }

    void DoPlayError(TInt aError)
    {
        // Fires when playback ends (we never set LastBuffer, so normally only on
        // a real error). No debug channel in this build.
        (void)aError;
    }

    void ToneFinished(TInt aError) { (void)aError; }
    void BufferToBeEmptied(CMMFBuffer *) {}
    void RecordError(TInt aError) { (void)aError; }
    void ConvertError(TInt aError) { (void)aError; }
    void DeviceMessage(TUid, const TDesC8 &) {}

private:
    CMMFDevSound *iDevSound;
    char *iMix;
    int iBlock, iCount;
    int *iProduced, *iConsumed;
    void (*iMixCb)(void);
};
#endif // Q_OS_SYMBIAN

// g_audioMutex is declared (once) in the forward-declaration block above.

// The MMF stream lives on a DEDICATED AUDIO THREAD (DevSound must be created
// and used from one thread); the worker receives commands as posted QEvents.
// Any wedge can only block the sacrificial worker, never the GUI. The mixer
// runs on the worker (mix-on-demand inside BufferToBeFilled); ring state is
// shared through g_audioMutex.
#ifdef Q_OS_SYMBIAN

// Worker command posted to the audio thread's event loop (plain QEvent subclass
// -> no Q_OBJECT, no moc, no queued-signal plumbing).
class AudioCmdEvent : public QEvent
{
public:
    enum { CmdOpen = 0, CmdRing, CmdStop };
    AudioCmdEvent(int c, char *p0 = 0, int p1 = 0, int p2 = 0, void *p3 = 0)
        : QEvent(static_cast<QEvent::Type>(QEvent::User + 1)),
          cmd(c), p0(p0), p1(p1), p2(p2), p3(p3) {}
    int cmd; char *p0; int p1, p2; void *p3;
};

// Owns the BelleMmfOutput. Lives on the audio thread (created in
// BelleAudioThread::run), so every MMF active object and callback runs there.
class BelleAudioWorker : public QObject
{
public:
    BelleAudioWorker() : iOut(0) {}
    bool event(QEvent *e)
    {
        if (e->type() == QEvent::User + 1) {
            AudioCmdEvent *ce = static_cast<AudioCmdEvent *>(e);
            switch (ce->cmd) {
            case AudioCmdEvent::CmdOpen:
                if (!iOut) {
                    iOut = new BelleMmfOutput();
                    iOut->OpenStream();
                }
                break;
            case AudioCmdEvent::CmdRing:
                if (iOut) {
                    iOut->SetRing(ce->p0, ce->p1, ce->p2,
                                  &g_audioProduced, &g_audioConsumed);
                    // v44: the mixer callback rides along with the ring so the
                    // worker can mix on demand in BufferToBeFilled.
                    if (ce->p3) iOut->SetMixCb((void (*)(void))ce->p3);
                }
                break;
            case AudioCmdEvent::CmdStop:
                if (iOut) { iOut->Close(); iOut = 0; }
                break;
            }
            return true;
        }
        return QObject::event(e);
    }
private:
    BelleMmfOutput *iOut;
};

class BelleAudioThread : public QThread
{
public:
    BelleAudioThread() : iWorker(0), iReady(false) {}
    // Wait for run() to create the worker (so the main thread never posts to a
    // half-constructed object). Returns once run() signals iReady.
    BelleAudioWorker *waitWorker(int timeoutMs)
    {
        iMutex.lock();
        if (!iReady) iCond.wait(&iMutex, timeoutMs);
        BelleAudioWorker *w = iWorker;
        iMutex.unlock();
        return w;
    }
protected:
    void run()
    {
        // Created HERE so the stream's active objects are serviced by this
        // thread's event loop (Qt 4.7 Symbian event dispatcher = active scheduler).
        iWorker = new BelleAudioWorker();
        iMutex.lock(); iReady = true; iMutex.unlock();
        iCond.wakeAll();
        exec();
        delete iWorker; iWorker = 0;
    }
private:
    QMutex iMutex; QWaitCondition iCond; bool iReady;
    BelleAudioWorker *iWorker;
};

static BelleAudioThread *g_audioThread = 0;
static BelleAudioWorker *g_audioWorker = 0;

// Lazily start the audio thread and wait (up to 3s) for the worker to exist.
static void startAudioWorker()
{
    if (g_audioThread) return;
    g_audioThread = new BelleAudioThread();
    // HighPriority: this worker is light -- mix + copy only, the OGG decode
    // runs on a dedicated BelleVorbisDecoder thread (NormalPriority), not here
    // -- so it never monopolises the single-core E7: it wakes at DevSound's
    // cadence (~0.3s) for a few ms of mixing and is idle the rest of the time.
    g_audioThread->start(QThread::HighPriority);
    g_audioWorker = g_audioThread->waitWorker(3000);
}

// Post a command to the audio worker (fire-and-forget; the worker processes it
// on the audio thread). No-op if the worker is unavailable.
static void postAudio(int cmd, char *p0 = 0, int p1 = 0, int p2 = 0, void *p3 = 0)
{
    if (!g_audioWorker) return;
    QCoreApplication::postEvent(g_audioWorker, new AudioCmdEvent(cmd, p0, p1, p2, p3));
}

// ======================================================================
// v48: background OGG decoder + ready-PCM ring
// ======================================================================
// stb_vorbis decodes on a DEDICATED NormalPriority thread into a ring of ready
// 16-bit-mono blocks; the mixer only memcpy()'s the next block out. The decoder
// thread is the ONLY user of stb_vorbis (via vorbis_decoder_read), so a heavy
// ~110ms soft-float decode can never stall DevSound or preempt the game thread
// in 100ms+ bursts -- the ~1.5s ring absorbs its scheduling latency instead.
struct BelleVorbisRing
{
    BelleVorbisRing(int slot_samples_, int nslots_)
        : slot_samples(slot_samples_), nslots(nslots_ > 32 ? 32 : nslots_),
          produced(0), consumed(0), eof(false), abort(false)
    {
        memset(slot_bufs, 0, sizeof(slot_bufs));
        memset(counts, 0, sizeof(counts));
        for (int i = 0; i < nslots; i++)
            slot_bufs[i] = (short *)malloc((size_t)slot_samples * sizeof(short));
    }
    ~BelleVorbisRing()
    {
        for (int i = 0; i < 32; i++)
            if (slot_bufs[i]) free(slot_bufs[i]);
    }
    QMutex         mutex;
    short         *slot_bufs[32];
    int            counts[32];
    int            slot_samples;
    int            nslots;
    int            produced, consumed;   // monotonic slot counters
    bool           eof;
    bool           abort;
};

// NormalPriority so it shares the single core fairly with the game thread and
// the Qt main thread; the ring absorbs the scheduling jitter. Only one runs at
// a time (the game plays one music track).
class BelleVorbisDecoder : public QThread
{
public:
    BelleVorbisDecoder(void *dec, BelleVorbisRing *ring, int loop)
        : m_dec(dec), m_ring(ring), m_loop(loop), m_abort(false) {}
    BelleVorbisRing *ring() const { return m_ring; }
    void requestStop() { m_abort = true; }
protected:
    void run()
    {
        short buf[4096];
        while (!m_abort) {
            int n = vorbis_decoder_read(m_dec, buf, 4096);
            if (n <= 0) {
                if (m_loop) { vorbis_decoder_seek(m_dec); continue; }
                belle_vorbis_ring_set_eof(m_ring);
                break;
            }
            if (!belle_vorbis_ring_produce(m_ring, buf, n)) break;  // aborted
        }
        belle_vorbis_ring_set_eof(m_ring);   // wake a consumer waiting on EOF
    }
private:
    void  *m_dec;
    BelleVorbisRing *m_ring;
    int    m_loop;
    volatile bool m_abort;
};

static BelleVorbisDecoder *g_vorbisDecoder = 0;

extern "C" void *belle_vorbis_ring_create(int slot_samples, int nslots)
{
    BelleVorbisRing *r = new BelleVorbisRing(slot_samples, nslots);
    for (int i = 0; i < r->nslots; i++)
        if (!r->slot_bufs[i]) { delete r; return 0; }
    return r;
}

extern "C" void belle_vorbis_ring_destroy(void *ring)
{
    delete (BelleVorbisRing *)ring;
}

extern "C" int belle_vorbis_ring_produce(void *ring, const short *samples, int n)
{
    BelleVorbisRing *r = (BelleVorbisRing *)ring;
    if (!r || n <= 0 || n > r->slot_samples) return 0;
    r->mutex.lock();
    /* v49: never block on the condition variable -- Qt 4.7 Symbian cross-thread
       wait/wakeAll latency can leave the producer asleep for seconds while the
       ring drains. Poll with a short sleep instead: the ring holds ~6s of
       music, so a few ms of scheduling jitter are irrelevant. */
    while (r->produced - r->consumed >= r->nslots && !r->abort) {
        r->mutex.unlock();
        belle_qt_sleep(5);   // QThread::msleep is protected in Qt 4.7; this helper wraps it
        r->mutex.lock();
    }
    if (r->abort) { r->mutex.unlock(); return 0; }
    const int slot = r->produced % r->nslots;
    memcpy(r->slot_bufs[slot], samples, (size_t)n * sizeof(short));
    r->counts[slot] = n;
    r->produced++;
    r->mutex.unlock();
    return 1;
}

extern "C" void belle_vorbis_ring_set_eof(void *ring)
{
    BelleVorbisRing *r = (BelleVorbisRing *)ring;
    if (!r) return;
    r->mutex.lock();
    r->eof = true;
    r->mutex.unlock();
}

extern "C" void belle_vorbis_ring_abort(void *ring)
{
    BelleVorbisRing *r = (BelleVorbisRing *)ring;
    if (!r) return;
    r->mutex.lock();
    r->abort = true;
    r->mutex.unlock();
}

extern "C" int belle_vorbis_ring_consume(void *ring, short *out)
{
    BelleVorbisRing *r = (BelleVorbisRing *)ring;
    if (!r) return 0;
    r->mutex.lock();
    if (r->produced == r->consumed) {
        const bool done = r->eof;
        r->mutex.unlock();
        return done ? 0 : -1;     // 0 = track over, -1 = underrun (repeat)
    }
    const int slot = r->consumed % r->nslots;
    const int n = r->counts[slot];
    memcpy(out, r->slot_bufs[slot], (size_t)n * sizeof(short));
    r->consumed++;
    r->mutex.unlock();
    return n;
}

extern "C" void belle_vorbis_spawn(void *dec, void *ring, int loop)
{
    BelleVorbisRing *r = (BelleVorbisRing *)ring;
    if (!r) return;
    // Prefill a couple of blocks synchronously so the first mixer request is
    // served instantly -- the thread's first wake-up can lag by tens of ms on
    // the single-core E7. Runs on the game thread (PlaySong); ~tens of ms.
    short pre[4096];
    for (int i = 0; i < 2; i++) {
        int n = vorbis_decoder_read(dec, pre, 4096);
        if (n <= 0) {
            if (loop) { vorbis_decoder_seek(dec); continue; }
            belle_vorbis_ring_set_eof(r);
            break;
        }
        belle_vorbis_ring_produce(r, pre, n);
    }
    BelleVorbisDecoder *th = new BelleVorbisDecoder(dec, r, loop);
    g_vorbisDecoder = th;
    th->start(QThread::NormalPriority);
}

extern "C" void belle_vorbis_shutdown(void *ring)
{
    BelleVorbisRing *r = (BelleVorbisRing *)ring;
    if (!r) return;
    belle_vorbis_ring_abort(r);             // wake a producer stuck on a full ring
    if (g_vorbisDecoder && g_vorbisDecoder->ring() == r) {
        g_vorbisDecoder->requestStop();
        g_vorbisDecoder->wait();            // join: the thread checks abort per block (~ms)
        delete g_vorbisDecoder;
        g_vorbisDecoder = 0;
    }
    belle_vorbis_ring_destroy(r);
}

#else
// Non-Symbian builds have no audio at all (OPEN/START below fail -> NoSound).
#endif

// Pending audio command (game thread -> main thread) plus its parameters.
enum { AUDIO_CMD_NONE = 0, AUDIO_CMD_OPEN, AUDIO_CMD_START,
       AUDIO_CMD_STOP, AUDIO_CMD_SHUTDOWN };
static QMutex g_audioCmdMutex;
static QWaitCondition g_audioCmdDone;
static volatile int g_audioCmd = AUDIO_CMD_NONE;
static volatile int g_audioCmdRet = -1;
static int g_audioCmdRate = 0, g_audioCmdCh = 0, g_audioCmdBits = 0;  // in: request; out: actual
static char *g_audioCmdBuf = 0;
static int g_audioCmdBlock = 0, g_audioCmdCount = 0;
static void (*g_audioCmdCb)(void) = 0;

// The mixer runs on the audio worker (mix-on-demand inside BufferToBeFilled),
// advancing every voice's position -- the game depends on that (the difficulty
// taunt spins in while(FX_SoundActive(handle)) until its voice ends). This paint
// tick only dispatches commands.
// (g_audioStarted is in the forward block above -- BelleMmfOutput reads it.)

// Executed on the Qt main thread at the top of paintEvent.
static void belle_audio_exec_cmds()
{
    g_audioCmdMutex.lock();
    const int cmd = g_audioCmd;
    g_audioCmdMutex.unlock();
    if (cmd == AUDIO_CMD_NONE) return;

    int ret = -1;
    switch (cmd) {
#ifdef Q_OS_SYMBIAN
    case AUDIO_CMD_OPEN:
        // The MMF stream is created and opened ON THE AUDIO WORKER THREAD. The
        // main thread only starts the worker and posts CmdOpen -- the
        // asynchronous open happens on the worker, so nothing here can wedge
        // the GUI. We return the format immediately (the game's MV_SetMixMode
        // re-tunes to it). If the open fails, the game simply stays silent
        // (mixer still runs).
        // 22050/mono. This value is what MV_SetMixMode re-tunes to (via
        // belle_audio_open), so mixer, ring and DevSound all agree on
        // 22050/mono: MV_BufferSize=512, MV_NumberOfBuffers=48 (ring 24576 B).
        g_audioCmdRate = 22050; g_audioCmdCh = 1; g_audioCmdBits = 16;
        startAudioWorker();
        postAudio(AudioCmdEvent::CmdOpen);
        ret = 0;
        break;
    case AUDIO_CMD_START:
        // Reset the ring counters under the mutex and hand the ring AND the
        // mixer callback to the worker (CmdRing). The worker then mixes on
        // demand inside BufferToBeFilled. The event runs on the worker AFTER
        // this function returns, so no MMF call happens on the main thread.
        g_audioMutex.lock();
        g_audioProduced = 0;
        g_audioConsumed = 0;
        g_audioMutex.unlock();
        g_audioStarted = 1;
        postAudio(AudioCmdEvent::CmdRing, g_audioCmdBuf, g_audioCmdBlock,
                  g_audioCmdCount, (void *)g_audioCmdCb);
        ret = 0;
        break;
#else
    case AUDIO_CMD_OPEN:
    case AUDIO_CMD_START:
        ret = -1;   // no audio on non-Symbian builds -> NoSound fallback
        break;
#endif
    case AUDIO_CMD_STOP:
        g_audioStarted = 0;
        // v41: the worker's Close() stops and deletes the CMMFDevSound on the
        // audio thread. A queued BufferToBeFilled can still arrive and is a safe
        // no-op while iDevSound==0; the next START opens a fresh DevSound
        // (OpenStream sees iDevSound==0).
#ifdef Q_OS_SYMBIAN
        postAudio(AudioCmdEvent::CmdStop);
#endif
        ret = 0;
        break;
    case AUDIO_CMD_SHUTDOWN:
        g_audioStarted = 0;
#ifdef Q_OS_SYMBIAN
        postAudio(AudioCmdEvent::CmdStop);
#endif
        ret = 0;
        break;
    default:
        ret = -1;
        break;
    }

    g_audioCmdMutex.lock();
    g_audioCmdRet = ret;
    g_audioCmd = AUDIO_CMD_NONE;
    g_audioCmdDone.wakeAll();
    g_audioCmdMutex.unlock();
}

// Post a command and wait for the main thread to execute it. The condition
// variable is re-checked under the mutex (classic lost-wakeup guard): wait is
// entered only while the command is still pending, and a 1.5s bound means a
// stalled GUI thread can never hang the game thread forever.
static int belle_audio_post_cmd(int cmd)
{
    g_audioCmdMutex.lock();
    g_audioCmdRet = -1;
    g_audioCmd = cmd;
    while (g_audioCmd != AUDIO_CMD_NONE) {
        if (!g_audioCmdDone.wait(&g_audioCmdMutex, 1500)) break;
    }
    const int ret = g_audioCmdRet;
    g_audioCmdMutex.unlock();
    return ret;
}

extern "C" int belle_audio_open(int *rate, int *channels, int *bits)
{
    if (!rate || !channels || !bits) return -1;
    g_audioCmdMutex.lock();
    g_audioCmdRate = *rate; g_audioCmdCh = *channels; g_audioCmdBits = *bits;
    g_audioCmdMutex.unlock();
    const int ret = belle_audio_post_cmd(AUDIO_CMD_OPEN);
    if (ret == 0) {
        g_audioCmdMutex.lock();
        *rate = g_audioCmdRate; *channels = g_audioCmdCh; *bits = g_audioCmdBits;
        g_audioCmdMutex.unlock();
    }
    return ret;
}

extern "C" int belle_audio_start(char *BufferStart, int BufferSize,
                                 int NumDivisions, void (*CallBackFunc)(void))
{
    g_audioCmdMutex.lock();
    g_audioCmdBuf = BufferStart; g_audioCmdBlock = BufferSize;
    g_audioCmdCount = NumDivisions; g_audioCmdCb = CallBackFunc;
    g_audioCmdMutex.unlock();
    return belle_audio_post_cmd(AUDIO_CMD_START);
}

extern "C" void belle_audio_stop(void)
{
    (void)belle_audio_post_cmd(AUDIO_CMD_STOP);
}

extern "C" void belle_audio_shutdown(void)
{
    (void)belle_audio_post_cmd(AUDIO_CMD_SHUTDOWN);
}

extern "C" void belle_audio_lock(void)   { g_audioMutex.lock(); }
extern "C" void belle_audio_unlock(void) { g_audioMutex.unlock(); }

int main(int argc, char **argv)
{
    // v-final: no sw.start marker (only the engine's own tekwar.log is written).
    // Ensure the game folder (BELLE_GAME_DIR from tekwar.pro, next to STUFF.DAT)
    // exists so the engine can open tekwar.log even before it reaches its own
    // mkdir in tekgame.c. mkdir() creates only one level, so the parent drive
    // root is made first.
    char belle_parent[256];
    strncpy(belle_parent, BELLE_GAME_DIR, sizeof(belle_parent)-1);
    belle_parent[sizeof(belle_parent)-1] = 0;
    {
        char *slash = strrchr(belle_parent, '/');
        if (slash) { *slash = 0; mkdir(belle_parent, S_IRWXU); }
    }
    mkdir(BELLE_GAME_DIR, S_IRWXU);

    QApplication app(argc, argv);

    g_clock.start();
    belle_ticks_ms = &qtMs;
    belle_ticks_us = &qtUs;

    GameWidget w;
    w.showFullScreen();

    // Pre-allocate the screen-sized scaled frame now, while memory is plentiful.
    // Doing it here (not on the first paint after the game sets its video mode)
    // keeps the fragile ~900KB allocation out of the window where the game
    // thread is simultaneously loading the level.
    g_scaled = QImage(640, 360, QImage::Format_RGB32);

    GameWorker worker;
    QObject::connect(&worker, SIGNAL(finished()), &app, SLOT(quit()));
    worker.start();

    int rc = app.exec();

    // v-final: no PROCESS EXITING marker (only the engine's own tekwar.log exists).
    return rc;
}
