/*
 * film.c - the soup engine, as it runs on the camera.
 *
 * The workflow is: pick a stock, shoot, and the JPEG appears. There is no
 * develop step to trigger -- a watcher task started at Magic Lantern's own
 * init notices new raws on the card and renders them.
 *
 * The watcher polls for new files rather than hooking Canon's capture path.
 * That is deliberate: an earlier build that read EDMAC registers around a
 * capture made the camera throw Err and lose the shot, so nothing here goes
 * anywhere near the imaging pipeline while it is running. Polling costs a
 * directory scan every couple of seconds and cannot disturb a capture.
 *
 * Entry points. tools/patch-film.py chains se_boot_init into the init task
 * and splices se_boot_key into the GUI task's event path; nothing else of
 * Magic Lantern's is involved any more.
 *
 *   se_boot_init  starts our two tasks, then tail-calls ML's own init
 *   se_watch      the watcher: scans for new raws and develops them
 *   se_screen     the interface, painted straight onto the panel
 *   se_key        every button, offered to us before Canon sees it
 *
 * The interface used to be drawn from inside Magic Lantern's menu redraw,
 * as a display callback installed into every one of its entries. It worked,
 * and it meant ML drew its own chrome around ours, the wheel cycled ML's
 * tabs underneath, and the menu flashed up before the film selector did.
 * None of that is fixable from inside somebody else's redraw, so the screen
 * is ours now: TRASH opens it, we light the panel, we own the palette, and
 * ML's menu never opens because we take that button.
 *
 * Nothing here writes a Canon property. Properties live in NVRAM and a bad
 * write is permanent, which is why the whole look is applied to pixel data
 * we own rather than by asking DIGIC to change its picture style.
 */
#include <stdarg.h>

/* Angle brackets, not quotes: a quoted include resolves against this file's
 * own directory first, which would always find the real camera header and
 * make it impossible to substitute the desktop simulator's. */
#include <cam.h>
#include "se.h"
#include "se_libc.h"
#include "cr2.h"
#include "stock.h"
#include "lut.h"
#include "jpeg_enc.h"
#include "pipeline.h"
#include "jpeg_dec.h"
#include "look.h"
#include "ui.h"

/* ===================================================================== */
/* Preconditions, as checks rather than as comments                        */
/* ===================================================================== */
/*
 * Every one of these was established in Phase 1 and every one of them is a
 * thing a previous build got wrong while a comment said otherwise. The
 * project's own rule: if you break a rule that is written in a comment,
 * the comment should have been an assertion.
 *
 * The geometry is Canon's, from CreatePhysicalVram (0xFFB0AFC0), which
 * fills its out-parameters with 720, 240 and 720 at 0xFFB0B040..0xFFB0B060
 * and then allocates 0x15184 bytes -- 86,400 of 4-bit pixels at 360 bytes
 * a row, plus four of slack. ML's bmp_idle_copy copies exactly 0x15180 of
 * them (its literal at 0x00980BB8) in rows of 360 (0x00980B6C).
 * docs/ML_DISPLAY.md 4.1, 3.4.
 */
#define SE_ASSERT_AT_BUILD(name, cond) \
    typedef char se_build_assert_##name[(cond) ? 1 : -1]

/* UI buffer geometry is shared; panel geometry comes from mlmap.h. */
SE_ASSERT_AT_BUILD(bmp_width,   SE_UI_W == 720);
SE_ASSERT_AT_BUILD(bmp_ui_rows, SE_UI_H == 240);
SE_ASSERT_AT_BUILD(bmp_ui_pitch, SE_UI_PITCH == 360);
SE_ASSERT_AT_BUILD(bmp_4bpp,    SE_UI_PITCH * 2 == SE_UI_W);
SE_ASSERT_AT_BUILD(bmp_bytes,   ML_BMP_BYTES == ML_BMP_PITCH * ML_BMP_ROWS);

SE_ASSERT_AT_BUILD(fake_btn,    (unsigned)SE_FAKE_BTN == 0xFFFE1DC0u);
SE_ASSERT_AT_BUILD(bgmt_menu,   SE_BGMT_MENU == SE_BTN_MENU);
SE_ASSERT_AT_BUILD(pal_mark,    SE_PALETTE_MARK == 0x03EB0000);
SE_ASSERT_AT_BUILD(pal_white,   SE_C_WHITE == 15);
SE_ASSERT_AT_BUILD(pal_clear,   SE_C_CLEAR == 0);
SE_ASSERT_AT_BUILD(pal_notml,   SE_PAL_MINE_INDEX != 15);
/* The screen must paint on every tick: Canon's own menu is underneath ours
 * and redraws on its own schedule. docs/ML_DISPLAY.md 13 (C6). */

/* ===================================================================== */
/* Menu state, exported to the patch tool through the header table         */
/* ===================================================================== */

#define SE_PATH_MAX 0x80       /* Canon's FIO path length limit */

/*
 * Canon's 16-entry overlay palette, and the word bmp_printf takes as its
 * first argument. 0x00030201 is the value ML's own Free Memory item uses,
 * which decodes as font 3, background 2, foreground 1 -- so the layout is
 * (font << 16) | (bg << 8) | fg. Taking it apart from working code beats
 * reconstructing ML's FONT() macro for this vintage.
 */
/*
 * These are Magic Lantern's DryOS colour indices, and they are NOT the same
 * numbers as engine/ui.h's SE_C_*.
 *
 * Two colour spaces meet in this file and telling them apart matters:
 *
 *   bmp_printf takes a DryOS index and converts it with D2V() before it
 *   writes a nibble, so anything passed to cam_printf uses these;
 *
 *   a frame blitted straight into the overlay buffer is not converted by
 *   anyone, so engine/ui.c uses the panel's own indices.
 *
 * Writing one where the other belonged is what made the first interface to
 * reach the camera come up red on green, so they have different names now.
 */
#define SE_ML_WHITE   0x01
#define SE_ML_BLACK   0x02
#define SE_ML_CYAN    0x05
#define SE_ML_GREEN   0x06
#define SE_ML_RED     0x08
#define SE_ML_YELLOW  0x0F
#define SE_FONT(fg, bg) (0x00030000u | ((unsigned)(bg) << 8) | (unsigned)(fg))


/* Button codes: SE_BTN_* from the platform cam.h. */

int se_stock_index = 0;
int se_res_index   = 0;        /* 0 = half size, 1 = the preview's own */
/*
 * Where a developed frame comes from. SE_SOURCE_PREVIEW grades the JPEG
 * DIGIC already embedded in the CR2; SE_SOURCE_RAW decodes the sensor data
 * and runs the whole pipeline. Either way one frame is developed right
 * after it is shot -- the difference is what is read, not when.
 *
 * Preview is the default and will stay the default. It is the only path
 * this body has ever finished a frame on, it costs a fifth of the reading,
 * and Canon's demosaic and colour correction are genuinely good. Raw is
 * here because owning every decision is sometimes the point.
 */
int se_src_index   = SE_SOURCE_PREVIEW;

/*
 * The chosen look, remembered across power cycles.
 *
 * ML/SE.CFG on the card, four lines of plain text. The stock is stored by
 * NAME, not by index: the list comes from whatever .fs files are in
 * ML/FILM, so an index would silently select a different film the moment
 * one is added or removed.
 *
 * NOTHING HERE WRITES THE CARD FROM A BUTTON. se_key runs on Canon's
 * GuiMainTask and may not touch the filesystem -- it sets se_cfg_dirty and
 * nothing else. The watcher notices and writes, which is the same division
 * the log already uses, and it means a wheel spun through eleven stocks
 * costs one card write rather than eleven.
 */
#define SE_CFG_PATH  "A:/ML/SE.CFG"
volatile int se_cfg_dirty;              /* set by se_key, cleared by the watcher */
static char  se_cfg_stock[SE_STOCK_NAME];
int se_auto_on     = 1;        /* watch for new frames and develop them */

/* Set from the menu to ask for a sweep of every raw already on the card. */
/* Not static: tools/camsim asserts SET does not set it. It used to, and
 * one thumb press would develop every frame on the card. */
volatile int se_request_all;

static volatile int se_ui_dirty;
volatile int se_trash_seen;             /* TRASH presses taken from ML */
void se_next_stock(void);
void se_prev_stock(void);
void se_toggle_source(void);

/*
 * The high-water mark: the greatest raw path on the card when the camera
 * started. Auto develop only looks past it, so switching the camera on does
 * not start grinding through a card full of older photos -- "develop the
 * frame I just took" is the whole point, and a full card would otherwise be
 * an hour of work and a great many card writes nobody asked for.
 *
 * Canon's own naming makes a single string enough: files within a folder
 * increase (IMG_0001 then IMG_0002), and a folder rollover increases too
 * ("101CANON/IMG_0001.CR2" sorts after "100CANON/IMG_9999.CR2"), so a plain
 * string comparison orders every frame by age. If the comparison is ever
 * wrong the failure is benign -- a frame is skipped, and "Develop all now"
 * ignores the mark entirely.
 */
static char se_watermark[SE_PATH_MAX];

const char se_str_stock_name[] = "Film stock";
const char se_str_stock_help[] = "Look applied when developing. From ML/FILM.";
const char se_str_dev_name[]   = "Develop all now";
const char se_str_dev_help[]   = "Also render the raws already on the card.";
const char se_str_res_name[]   = "Resolution";
const char se_str_res_help[]   = "Output size of the developed JPEG.";
const char se_str_auto_name[]  = "Auto develop";
const char se_str_auto_help[]  = "Render each frame as you shoot it.";

/* The watcher task's entry stub, in start.S: it switches to the worker
 * stack before calling se_watch. */
extern void se_boot_watch(void);
extern void se_boot_screen(void);
void se_start(void);

/*
 * The lowest words of the worker stack, filled with a pattern at startup and
 * checked after every develop. A stack overflow here does not crash -- it
 * quietly rewrites whatever is below and the damage shows up as corrupt
 * pixels much later, which cost several trips to the camera to find once
 * already. This turns the next one into a line in the log.
 */
extern char __wstack_bottom[];
#define SE_CANARY_WORDS 16
#define SE_CANARY_VALUE 0xFE5CA1EDu

static void se_canary_set(void)
{
    volatile uint32_t *p = (volatile uint32_t *)(void *)__wstack_bottom;
    int i;
    for (i = 0; i < SE_CANARY_WORDS; i++) p[i] = SE_CANARY_VALUE;
}

static int se_canary_ok(void)
{
    volatile uint32_t *p = (volatile uint32_t *)(void *)__wstack_bottom;
    int i;
    for (i = 0; i < SE_CANARY_WORDS; i++)
        if (p[i] != SE_CANARY_VALUE) return 0;
    return 1;
}

#define SE_MAX_STOCKS 24

static se_stock se_stocks[SE_MAX_STOCKS];
static int      se_nstocks = -1;        /* -1 = not scanned yet */

/* Progress, shown on the menu line while a develop runs. */
/*
 * Not static: tools/camsim waits for the develop to finish before injecting
 * its simulated capture.
 *
 * The watcher's outer loop runs se_sweep() -- which develops a frame, and
 * takes seconds -- and only then runs the inner loop that polls the imaging
 * buffers. So a content change injected while a develop is in flight is not
 * sampled until the develop ends, and the simulator's capture test went red
 * about one run in six because its window closed first. The screen painting
 * ten times a second made that window tighter, which is how it surfaced.
 *
 * The test wants to know whether the screen interferes with a capture, not
 * whether a develop and a capture can be made to overlap on a host with
 * host timing, so it should wait for the state it means to test.
 */
volatile int se_busy;

/* The last successful develop's look checksum. See develop_preview. */
volatile uint32_t se_last_look_hash;
static volatile int  se_pct;
static const char   *se_stage  = "";
static char          se_status[64];
static int           se_done, se_failed;

/*
 * This sweep's queue, for "FRAME 2 OF 5" on the working screen.
 *
 * se_uistate.total was never assigned by anything, so the screen read
 * "FRAME 1 OF 0" through every develop -- the one number on it that says
 * whether there is more coming. se_sweep already counts the queue: its
 * probe pass walks the card with j.probe set precisely to find out how
 * many frames are waiting before it commits any memory, and then threw
 * the answer away.
 *
 * Separate from se_done, which counts the life of the session and is what
 * the log and the status line report.
 */
static volatile int  se_q_total, se_q_done;

/* How long the card actually took to answer at startup, in ms. Logged so
 * the five-second cap below can be judged against what really happens. */
static int           se_card_ms;
static int           se_last_secs;        /* how long the last frame took */

/* ===================================================================== */
/* Small helpers                                                          */
/* ===================================================================== */

static void se_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, n, fmt, ap);
    va_end(ap);
}

static void str_copy(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    if (!n) return;
    while (src[i] && i + 1 < n) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Append and return the new end, so a line can be built left to right
 * without a length calculation per piece. */
static char *put_s(char *d, const char *s)
{
    while (*s) *d++ = *s++;
    *d = '\0';
    return d;
}

/* Case-insensitive equality, for matching a remembered stock name against
 * the list actually on the card. */
static int se_streq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    size_t i;
    if (lx > ls) return 0;
    for (i = 0; i < lx; i++) {
        char a = s[ls - lx + i], b = suffix[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return 1;
}

/* ===================================================================== */
/* The log                                                                */
/* ===================================================================== */
/*
 * A:/ML/SE.LOG, rewritten in full on every append.
 *
 * The camera gives almost nothing back when something goes wrong: a task
 * that faults just stops, and anything drawn to the screen can be wiped by
 * Canon's own redraw before it is read. A file on the card is the one
 * channel that survives both, and it can be read on a desktop afterwards.
 * Rewriting the whole buffer each time avoids needing append semantics from
 * Canon's FIO, which are not something to guess at.
 */
/*
 * 4 KB, not 1 KB. The probe writes a dozen lines of its own and the old
 * buffer filled before the develop had anything to say, so the lines that
 * mattered were the ones silently dropped.
 */
/*
 * 8 KB, and a bigger pin.
 *
 * The develop-seam probe writes four lines per report and they arrive early
 * -- which, with a rolling buffer that drops the oldest line after the pin,
 * made exactly the lines worth reading the first to go. The pin now covers
 * the boot story and the first probe report; everything after it rolls.
 */
#define SE_LOG_MAX 16384
#define SE_LOG_PIN 1536        /* the opening lines, never rolled away */
static char se_logbuf[SE_LOG_MAX];
static int  se_loglen;
static volatile int se_log_busy;
static int          se_log_alt;

/*
 * Is Canon shutting down?
 *
 * ML's own tasks all stop on this and ours did not, which is why the card
 * LED stayed lit after the camera was switched off: the watcher kept
 * walking directories and writing the log, and the screen kept blitting
 * into a VRAM buffer that Canon's teardown frees. See CAM_ml_shutdown.
 */
static int se_shutting_down(void)
{
    return MEM(CAM_ml_shutdown) != 0;
}

static void se_note(const char *fmt, ...)
{
    va_list ap;
    int h, n;

    /*
     * The whole function under the lock, not just the file write.
     *
     * Guarding only the write left the buffer append unguarded, and the
     * camera proved that is not enough: a log came back reading
     * "probe: cpsrprobe: buf=0x000F5F04" -- one task's line cut in half by
     * another's. A dropped line is merely absent; a garbled one corrupts
     * its neighbour as well and makes the whole file untrustworthy, which
     * is the worse failure in the one channel that survives a task dying.
     *
     * A task that cannot take the lock abandons its line rather than
     * waiting for it. Waiting would let a logging call block the develop
     * behind the watcher, and no line is worth that.
     */
    /* Not one more card write once Canon has asked to shut down. This is
     * the same test ML puts first in EngDrvOut (0x00989118). */
    if (se_shutting_down()) return;
    if (cam_test_and_set(&se_log_busy)) return;

    /*
     * Make room rather than going quiet.
     *
     * This used to refuse to append once the buffer was nearly full, which
     * meant the log stopped at whatever happened first and the lines worth
     * reading -- the ones from just before the camera was switched off --
     * were the ones never written. Now whole lines are dropped to make room.
     *
     * The first few hundred bytes are pinned, because the opening lines say
     * what was loaded, how many stocks were found and whether the display
     * was installed, and that story does not repeat. Everything after the
     * pin rolls.
     */
    while (se_loglen > SE_LOG_MAX - 160) {
        int i = SE_LOG_PIN, j;
        while (i < se_loglen && se_logbuf[i] != '\n') i++;
        if (i >= se_loglen) { se_loglen = SE_LOG_PIN; break; }
        i++;                                   /* past the newline */
        for (j = SE_LOG_PIN; i < se_loglen; j++, i++)
            se_logbuf[j] = se_logbuf[i];
        se_loglen = j;
        se_logbuf[se_loglen] = '\0';
    }

    va_start(ap, fmt);
    n = vsnprintf(se_logbuf + se_loglen, (size_t)(SE_LOG_MAX - se_loglen - 2),
                  fmt, ap);
    va_end(ap);
    if (n < 0) { cam_release(&se_log_busy); return; }
    se_loglen += n;
    if (se_loglen > SE_LOG_MAX - 2) se_loglen = SE_LOG_MAX - 2;
    se_logbuf[se_loglen++] = '\n';
    se_logbuf[se_loglen]   = '\0';

    /*
     * Two files, written alternately.
     *
     * The log is rewritten in full every time, so there is always a moment
     * when the file on the card is empty -- and switching a camera off is
     * not a considered act. A whole session's diagnostics was lost to
     * exactly that: SE.LOG came back zero bytes while four frames sat
     * beside it, correctly developed, with nothing to say how.
     *
     * Alternating means at most one of the pair is ever mid-truncate, and
     * the other holds the previous complete copy -- one line behind at
     * worst. The reader takes whichever is longer.
     */
    h = cam_create(se_log_alt ? "A:/ML/SE.LO2" : "A:/ML/SE.LOG");
    if (h != -1) {
        cam_write(h, se_logbuf, (size_t)se_loglen);
        cam_close(h);
        se_log_alt = !se_log_alt;
    }
    cam_release(&se_log_busy);
}

/*
 * Read exactly `n` bytes from `off`, however many calls that takes.
 *
 * Two things this must survive, both of which Canon's FIO does and neither
 * of which is documented:
 *
 *   - a read returning less than was asked for. Treating that as failure
 *     made the decoder's window refill give up, so it padded the rest of
 *     the frame with 1-bits and carried on, returning success while
 *     emitting garbage. That was the corrupt-JPEG bug.
 *
 *   - a read returning zero without being at end of file. Treating that as
 *     end of file truncated stock files, so whichever one had its NAME line
 *     furthest in failed to parse -- a different one each run, depending on
 *     where the boundary happened to fall.
 *
 * Both are reproduced by the desktop simulator (SE_SIM_SHORT_READS,
 * SE_SIM_ZERO_READS), which is how they were found without the camera.
 *
 * Every read is positioned explicitly rather than relying on the file's
 * current offset, so a retry cannot drift, and a zero is retried a few times
 * before being believed.
 */
/*
 * How hard to try when a read says "nothing right now".
 *
 * Four immediate retries was not enough and the cost of that was severe. A
 * busy card can say it many times running, and four attempts with no pause
 * between them is barely a pause at all -- so read_at gave up mid-frame,
 * the decoder treated the short read as the end of the picture, and a
 * plausible, wrong JPEG was written and reported as a success.
 *
 * The decoder reports that honestly now, but the right answer to a card
 * that is merely busy is to wait for it. Forty tries with five milliseconds
 * between them is a fifth of a second of patience per read, against a
 * develop that takes forty seconds; when the card is healthy none of it is
 * spent, because the first read succeeds.
 */
#define SE_READ_RETRIES 40
#define SE_READ_PAUSE_MS 5

/*
 * The one buffer Canon's DMA is ever pointed at.
 *
 * It is written only by FIO and read only through the uncached alias, so it
 * never has a dirty cache line to be evicted over the top of a later read.
 * Everything above this function gets its data by ordinary cached stores
 * into its own buffer, which DMA never touches -- so each side is accessed
 * one way only and neither can surprise the other.
 *
 * 8 KB, which is the decoder's window size, so a refill is one call. Larger
 * requests -- the 256 KB header read -- are simply chunked, which costs a
 * few more FIO calls and nothing else.
 */
#define SE_BOUNCE 8192
static uint8_t se_bounce[SE_BOUNCE] __attribute__((aligned(32)));

static int read_at(int h, int off, void *buf, int n)
{
    uint8_t *p = (uint8_t *)buf;
    int done = 0, idle = 0;

    while (done < n) {
        int want = n - done, k;
        if (want > SE_BOUNCE) want = SE_BOUNCE;
        if (cam_seek(h, off + done, 0) < 0) break;
        k = cam_read(h, CAM_UNCACHED(se_bounce), (size_t)want);
        if (k > 0) {
            cam_copy_uncached(p + done, se_bounce, k);
            done += k; idle = 0; continue;
        }
        if (k < 0) break;                      /* a real error */
        if (++idle > SE_READ_RETRIES) break;   /* genuinely the end */
        cam_msleep(SE_READ_PAUSE_MS);          /* let the card catch up */
    }
    return done;
}

/* Read a whole file into `buf`. Returns bytes read, or -1. */
static int read_file(const char *path, void *buf, size_t cap)
{
    uint32_t sz = 0;
    int h, got;

    /*
     * Ask how big it is first, so read_at is never asked for more than
     * exists.
     *
     * read_at loops until it has the full count it was given, and it
     * cannot tell a card that is busy from a file that has ended -- both
     * return zero -- so it waits SE_READ_RETRIES times at
     * SE_READ_PAUSE_MS before giving up. That is 200 ms, and EVERY file
     * smaller than its caller's buffer paid it: the thirteen stocks are
     * about 4 KB each and were read with a cap of 8000.
     *
     * Three seconds of pure sleeping on every boot -- and the camera
     * restarts coming out of standby, so three seconds every time it woke
     * up, with the card LED blinking through all of it.
     */
    CAM_CALL2(CAM_FIO_GetFileSize, int, path, &sz);
    if (sz > 0 && (size_t)sz < cap) cap = (size_t)sz;

    h = cam_open(path);
    if (h == -1) return -1;
    got = read_at(h, 0, buf, (int)cap);
    cam_close(h);
    return got;
}

/* ===================================================================== */
/* Directory scanning                                                     */
/* ===================================================================== */

#define ATTR_DIRECTORY 0x10

/*
 * What FIO_FindFirstEx fills on DIGIC 4, from Magic Lantern's fio-ml.h
 * under CONFIG_DIGIC_45 (the 50D is DIGIC IV, and that is the layout the
 * 50D.109 build compiles). The name sits four bytes later than on the
 * 5D Classic. A walk that used the old offset would list garbage.
 */
struct fio_file {
    uint16_t mode;          /* FAT attributes; 0x10 = directory */
    uint16_t pad;
    uint32_t size;
    uint32_t timestamp;
    uint32_t unk;
    char     name[256];
};

typedef void (*se_walk_fn)(const char *dir, const struct fio_file *f, void *ctx);

/*
 * Canon's FIO_FindFirstEx returns a cookie, negative on failure. The
 * iteration contract is the one Magic Lantern uses throughout: the first
 * entry is already in `f` when FindFirstEx returns, so the loop body runs
 * before the first FindNextEx.
 */
static long se_find_first(const char *dir, struct fio_file *f)
{
    return CAM_CALL2(CAM_FIO_FindFirstEx, long, dir, f);
}

/*
 * Walk a directory.
 *
 * The path is tried both with and without its trailing slash. Which form
 * Canon's opendir wants here is not documented anywhere we can check, and
 * getting it wrong is silent: FindFirstEx just reports failure, the walk
 * finds nothing, and no frame is ever developed. Trying both costs one
 * failed call and removes the guess.
 */
static int se_walk(const char *dir, se_walk_fn fn, void *ctx)
{
    struct fio_file f;
    char  alt[SE_PATH_MAX];
    long  d;
    int   n = 0;

    d = se_find_first(dir, &f);
    if ((unsigned long)d < 0x10000) {
        size_t l = strlen(dir);
        if (l && l < sizeof(alt) && dir[l - 1] == '/') {
            str_copy(alt, sizeof(alt), dir);
            alt[l - 1] = '\0';
            d = se_find_first(alt, &f);
        }
    }
    if ((unsigned long)d < 0x10000) {
        se_note("walk %s FAILED (%ld)", dir, d);
        return -1;
    }

    do {
        if (f.name[0] == '.' || f.name[0] == '_') continue;
        n++;
        fn(dir, &f, ctx);
    } while (CAM_CALL2(CAM_FIO_FindNextEx, int, d, &f) == 0);
    CAM_CALL1(CAM_FIO_FindClose, void, d);

    /*
     * Only say something when the count changes. se_note rewrites the whole
     * log file, so logging every walk meant a card write every couple of
     * seconds for the life of the camera -- which is what kept the access
     * LED blinking continuously, and buried the interesting lines in
     * hundreds of identical ones.
     */
    /*
     * ...and `last` has to be PER DIRECTORY.
     *
     * One static was shared between the two this is called for -- DCIM
     * with one entry and DCIM/100EOS5D with a hundred and fifty -- so the
     * count always differed from the previous call and every poll logged
     * both, which is precisely what the guard above was written to stop.
     * A session's log came back with eighteen identical walk lines in it.
     *
     * Keyed on a hash of the path rather than the pointer, because one
     * caller passes a literal and another passes a buffer it rebuilt.
     */
    {
        static unsigned hsh[4];
        static int      cnt[4];
        unsigned        h = 2166136261u;
        const char     *q = dir;
        int             k, slot = -1;

        while (*q) h = (h ^ (unsigned char)*q++) * 16777619u;
        if (!h) h = 1;                      /* 0 marks an unused slot */
        for (k = 0; k < 4; k++) if (hsh[k] == h) { slot = k; break; }
        if (slot < 0)
            for (k = 0; k < 4; k++)
                if (!hsh[k]) { hsh[k] = h; cnt[k] = -1; slot = k; break; }

        if (slot < 0 || cnt[slot] != n) {
            se_note("walk %s -> %d entries", dir, n);
            if (slot >= 0) cnt[slot] = n;
        }
    }
    return 0;
}

/* ------------------------------------------------------- the stock list */

/*
 * Load the stock list.
 *
 * This reads ML/FILM/INDEX.TXT -- one filename per line, written by
 * `filmdev card` -- rather than enumerating the directory. Directory
 * enumeration was the first thing the payload did and the last thing it
 * ever logged: SE.LOG from the camera ended at "payload entered", meaning
 * it died somewhere in here, and FIO_FindFirstEx is the one call in it
 * whose contract is guessed rather than known. Its cookie type, its failure
 * value and the exact shape of the dirent it fills all come from a newer
 * Magic Lantern tree than the binary this runs inside.
 *
 * Opening files by name needs none of that, and SE.LOG proves plain file
 * I/O already works from this task. An index also makes the menu order
 * something the desktop side chooses rather than whatever order the
 * filesystem happens to return.
 */
/*
 * Load and save ML/SE.CFG.
 *
 * Deliberately the dumbest format that works: "KEY value" a line. The card
 * is user-visible and someone will edit this by hand, so every value is
 * re-clamped on the way in and an unreadable file is simply no file --
 * the defaults stand and nothing complains. A truncated write (the camera
 * switched off mid-save) must not be able to select a stock that is not
 * there, which is why the stock is matched by name against the list that
 * was actually loaded and ignored if it does not match.
 */
/*
 * The chosen film, by name. Not static: tools/camsim prints it so the
 * suite can check ML/SE.CFG actually round-trips across a power cycle.
 * An accessor rather than exporting the array, so the mock does not have
 * to know what a se_stock looks like.
 */
const char *se_active_stock_name(void)
{
    if (se_nstocks <= 0 || se_stock_index < 0 || se_stock_index >= se_nstocks)
        return "(none)";
    return se_stocks[se_stock_index].name;
}

static void se_cfg_save(void)
{
    char buf[160], *d;
    int  h;

    if (se_shutting_down()) return;

    d = buf;
    d = put_s(d, "STOCK ");
    d = put_s(d, (se_nstocks > 0 && se_stock_index < se_nstocks)
                 ? se_stocks[se_stock_index].name : "");
    d = put_s(d, "\nSOURCE ");
    d = put_s(d, se_src_index == SE_SOURCE_RAW ? "RAW" : "JPEG");
    d = put_s(d, "\nSIZE ");
    d = put_s(d, se_res_index ? "FULL" : "HALF");
    d = put_s(d, "\n");

    h = cam_create(SE_CFG_PATH);
    if (h == -1) { se_note("cfg: cannot write " SE_CFG_PATH); return; }
    cam_write(h, buf, (size_t)(d - buf));
    cam_close(h);
    se_note("cfg saved: %s / %s / %s",
            (se_nstocks > 0 && se_stock_index < se_nstocks)
                ? se_stocks[se_stock_index].name : "(none)",
            se_src_index == SE_SOURCE_RAW ? "RAW" : "JPEG",
            se_res_index ? "FULL" : "HALF");
}

/* Is `line` the key `key`? Returns the value, or 0. */
static const char *cfg_val(const char *line, const char *key)
{
    int i;
    for (i = 0; key[i]; i++)
        if (line[i] != key[i]) return 0;
    if (line[i] != ' ') return 0;
    return line + i + 1;
}

static int cfg_is(const char *v, const char *want)
{
    int i;
    for (i = 0; want[i]; i++) if (v[i] != want[i]) return 0;
    return v[i] == '\0' || v[i] == ' ';
}

static void se_cfg_load(void)
{
    char text[256];
    int  got, i, line0;

    got = read_file(SE_CFG_PATH, text, sizeof(text) - 1);
    if (got <= 0) { se_note("cfg: none, using defaults"); return; }
    text[got] = '\0';

    line0 = 0;
    for (i = 0; i <= got; i++) {
        const char *v, *line;
        if (text[i] != '\n' && text[i] != '\r' && text[i] != '\0') continue;
        text[i] = '\0';
        line = text + line0;
        line0 = i + 1;
        if (!line[0]) continue;

        if ((v = cfg_val(line, "STOCK")) != 0) {
            str_copy(se_cfg_stock, sizeof(se_cfg_stock), v);
        } else if ((v = cfg_val(line, "SOURCE")) != 0) {
            se_src_index = cfg_is(v, "RAW") ? SE_SOURCE_RAW : SE_SOURCE_PREVIEW;
        } else if ((v = cfg_val(line, "SIZE")) != 0) {
            se_res_index = cfg_is(v, "FULL") ? 1 : 0;
        }
        /* No GRAIN key. The UI has a grain row, but the payload never
         * reads it -- grain comes from the stock file's own GRAIN line and
         * is part of what a stock IS. Persisting a setting that does
         * nothing would be worse than not having it. */
    }
    se_note("cfg read: stock '%s' source %s size %s",
            se_cfg_stock, se_src_index == SE_SOURCE_RAW ? "RAW" : "JPEG",
            se_res_index ? "FULL" : "HALF");
}

/* Once the stocks are loaded, resolve the remembered name to an index. */
static void se_cfg_apply_stock(void)
{
    int i;
    if (!se_cfg_stock[0] || se_nstocks <= 0) return;
    for (i = 0; i < se_nstocks; i++)
        if (se_streq_ci(se_stocks[i].name, se_cfg_stock)) {
            se_stock_index = i;
            se_note("cfg: '%s' is stock %d", se_cfg_stock, i);
            return;
        }
    se_note("cfg: '%s' is not on the card any more, keeping %s",
            se_cfg_stock,
            se_nstocks ? se_stocks[se_stock_index].name : "(none)");
}

static void se_scan_stocks(void)
{
    char  *text;
    char   path[SE_PATH_MAX];
    int    got, i, line0;

    if (se_nstocks >= 0) return;
    se_nstocks = 0;

    se_note("scan: allocating");
    text = (char *)cam_alloc(16384);
    if (!text) { se_note("scan: alloc FAILED"); goto fallback; }
    se_note("scan: allocated");

    got = read_file("A:/ML/FILM/INDEX.TXT", text, 8192);
    se_note("scan: index read %d", got);
    if (got <= 0) goto done;
    text[got] = '\0';

    /* Walk the index line by line, loading each named stock. */
    line0 = 0;
    for (i = 0; i <= got; i++) {
        char *name, *body;
        int   n;

        if (text[i] != '\n' && text[i] != '\r' && text[i] != '\0') continue;
        text[i] = '\0';
        name = text + line0;
        line0 = i + 1;
        if (!name[0]) continue;
        if (se_nstocks >= SE_MAX_STOCKS) break;

        se_snprintf(path, sizeof(path), "A:/ML/FILM/%s", name);
        body = text + 8192;                 /* second half of the buffer */
        n = read_file(path, body, 8000);
        if (n <= 0) { se_note("scan: %s unreadable", name); continue; }

        se_stock_init(&se_stocks[se_nstocks]);
        if (se_stock_parse(body, (size_t)n, &se_stocks[se_nstocks]) != SE_OK) {
            se_note("scan: %s unparsable", name);
            continue;
        }
        if (!se_stocks[se_nstocks].name[0])
            str_copy(se_stocks[se_nstocks].name, SE_STOCK_NAME, name);
        se_nstocks++;
    }

done:
    cam_free(text);
fallback:
    if (se_nstocks == 0) {
        /* No index, or nothing in it: offer a straight render so the
         * selector still works rather than showing an empty list. */
        se_stock_init(&se_stocks[0]);
        str_copy(se_stocks[0].name, SE_STOCK_NAME, "Standard");
        str_copy(se_stocks[0].desc, SE_STOCK_DESC, "no ML/FILM/INDEX.TXT");
        se_nstocks = 1;
    }
    if (se_stock_index >= se_nstocks) se_stock_index = 0;
}

/* ===================================================================== */
/* Living inside Magic Lantern's menu                                     */
/* ===================================================================== */
/*
 * The 5D Classic has no live view. Its rear screen is only on for Canon's
 * own menus and playback, and for Magic Lantern's menu -- and
 * run_in_separate_task closes the ML menu before it runs us. So the first
 * version drew a full screen into a buffer that nothing was displaying,
 * which is why redrawing it on a timer changed nothing: the panel was dark.
 *
 * The fix is to be part of the menu rather than to replace it. Once the
 * payload is loaded and has proved it can run, it points this entry's
 * display and select callbacks at itself, so the stock list is drawn by the
 * same code path that draws every other ML menu line -- which is, by
 * definition, visible.
 *
 * Doing this from C at runtime, rather than in the patch tool, is what keeps
 * it safe: until the payload is actually running, ML's menu holds no pointer
 * into it at all.
 */
/*
 * Drawing, without depending on Magic Lantern's menu internals.
 *
 * The first attempt rewrote the display pointers of all nine Debug entries.
 * Two fields of that struct are certain, because the camera proves them
 * every time it starts: the loader is reached through entry 1's priv at
 * +0x08 and its select at +0x1C. The display slot at +0x28 was never
 * confirmed, only inferred -- and reading the renderer shows why nothing
 * ever drew. Its loop reads +0x08, +0x0C, +0x20, +0x2C, +0x30, +0x32, +0x34
 * and +0x3C, and never +0x28 at all. It also stops as soon as the page is
 * full --
 *
 *     add r9, r9, #1        ; rows drawn
 *     ldr ip, [sp, #12]     ; the page's row limit
 *     cmp r9, ip
 *     bge <done>
 *
 * -- so entries past the fold are never visited anyway. Between a slot the
 * renderer does not consult and a loop that stops early, nothing of ours
 * was ever going to run.
 *
 * So nothing is patched into the menu now. The watcher task simply paints
 * the screen on a timer. The panel is dark unless ML's menu is open, so this
 * costs nothing visible the rest of the time; when the menu is open our
 * painting lands on top of it. Magic Lantern keeps doing the two things only
 * it can -- lighting the panel and delivering buttons -- and we stop making
 * assumptions about its data structures.
 */

/* ------------------------------------------------- getting on the screen */
/*
 * Why nothing drawn here has ever appeared, settled by reading the binary
 * this payload is patched into rather than by another trip to the camera.
 *
 * bmp_puts, which bmp_printf tail-calls, starts:
 *
 *     r0 = bmp_vram();  if (!r0 || (r0 & 1)) return;
 *
 * and bmp_vram (0x00980BBC) is:
 *
 *     return MEM(0x009C4068) ? MEM(0x009C856C)    the idle buffer
 *                            : MEM(0x00029328);   the one on the panel
 *
 * ML's menu redraw (menu_redraw_do, 0x00986668) sets that flag, draws the
 * whole page into the idle buffer, clears the flag, and then copies all
 * 86,400 bytes of idle over the real buffer. So:
 *
 *   - drawing from a task of our own writes the real buffer, and ML's next
 *     redraw wipes every byte of it. That is the timer paint, and it could
 *     never have worked;
 *   - drawing while the panel is dark writes a buffer nothing is scanning
 *     out, and MEM(0x00029328) is not even guaranteed to be a buffer then.
 *
 * The only place a draw survives is *inside* ML's redraw, between the two
 * writes to the idle flag. ML offers exactly that: every entry on the page
 * may carry a display callback, which menus_display calls while the idle
 * flag is set, and ML then blits the result to the panel itself.
 *
 *     ldr ip, [r4, #0x2C]            the display slot
 *     cmp ip, #0
 *     beq <ML's own row renderer>
 *     ldr r0, [r4, #0x0C]            priv
 *     mov r1, x ; mov r2, y ; ldrb r3, [r4, #0x08]
 *     mov lr, pc ; bx ip
 *
 * This route was tried once and written off. The note said the renderer
 * "never reads +0x28" -- but the entry list starts at 0x009B16CC, four bytes
 * below the figure in the notes, so the slot called +0x28 there is this
 * +0x2C, and it is read on every pass. The addresses the old patch wrote
 * were right; what was wrong was the conclusion drawn when nothing appeared.
 *
 * So: no code of ML's is rewritten any more. One word per entry, pointing at
 * our own veneer, and ML does the drawing. Nothing can wedge the camera,
 * because nothing of ML's instruction stream changes.
 */

/*
 * A deliberately small first step. Before any of the film interface exists,
 * this answers the one question everything else rests on: can a single
 * string be put on the panel at all?
 *
 * It is built to be worth a camera trip whichever way it goes. The callback
 * draws, then reads its own pixels back out of the buffer ML is about to
 * blit, and records what it found. So the log distinguishes the three
 * outcomes that have been indistinguishable until now:
 *
 *   calls == 0            ML never called us: the slot or the offset is wrong
 *   calls > 0, changed=0  we were called and bmp_printf drew nothing
 *   calls > 0, changed=N  our pixels reached the buffer on the panel
 *
 * and in the last case, whether anything is visible is then a question about
 * the panel and not about us.
 */
/*
 * One frame of the interface, 360 x 240 bytes, from Canon's heap.
 *
 * Allocated once by the watcher -- on a task of ours, where allocating is
 * -- and kept for the life of the camera. The display callback renders into
 * it and must never allocate: it runs inside ML's redraw on ML's menu task,
 * where the only things permitted are arithmetic and writes to the buffer.
 *
 * 84 KB against the 2264 KB the body reports free, with the develop peaking
 * at 592 KB. Allocating and freeing it per frame would churn a heap whose
 * fragmentation has already cost this project a develop.
 */
static uint8_t    *se_fb;
static se_ui       se_uistate;
static se_ui_stock se_uilist[SE_MAX_STOCKS];

/*
 * Fill in the interface's state from the engine's, and draw a frame.
 *
 * Pure computation into our own buffer: no allocation, no card, nothing that
 * can block. That is what makes it safe to call from inside ML's redraw.
 */
static void se_render(void)
{
    se_ui_fb fb;
    int i;

    for (i = 0; i < se_nstocks && i < SE_MAX_STOCKS; i++) {
        se_uilist[i].name = se_stocks[i].name;
        se_uilist[i].desc = se_stocks[i].desc;
    }
    se_ui_set_stocks(&se_uistate, se_uilist, se_nstocks > 0 ? se_nstocks : 0);
    se_uistate.stock   = se_stock_index;
    /*
     * The settings the interface shows are the engine's own, copied every
     * frame. They used to be whatever se_ui_init left behind, so the
     * screen could say one thing while the develop did another.
     */
    se_uistate.value[SE_SET_SOURCE] = se_src_index;
    se_uistate.value[SE_SET_RES]    = se_res_index ? 0 : 1;   /* FULL : HALF */
    se_uistate.screen  = se_busy ? SE_SCREEN_WORKING : SE_SCREEN_STOCK;
    se_uistate.percent = se_pct;
    se_uistate.stage   = se_stage;
    se_uistate.done    = se_q_done;
    se_uistate.total   = se_q_total;
    se_uistate.last_seconds = se_last_secs;
    str_copy(se_uistate.file, sizeof(se_uistate.file), se_status);

    fb.px = se_fb;
    se_ui_draw(&se_uistate, &fb);
}

/*
 * What the interface would look like right now, as one number.
 *
 * ML redraws its menu constantly -- the camera logged 1757 calls into the
 * display callback in a single session -- and re-rendering an identical
 * frame each time is 86 KB of writes and a screenful of glyphs for nothing,
 * on the menu task, while the worker is developing. The blit still has to
 * happen every pass, because ML clears the idle buffer with bmp_fill before
 * it calls us; only the drawing can be skipped.
 *
 * The percentage is quantised so that a develop, which moves it constantly,
 * does not defeat the whole point.
 */
static uint32_t se_ui_sig(void)
{
    return (uint32_t)se_stock_index * 7919u
         + (uint32_t)se_src_index * 65537u
         + (uint32_t)se_res_index * 257u
         + (uint32_t)(se_busy ? 1 : 0) * 104729u
         + (uint32_t)(se_pct / 5) * 131u
         + (uint32_t)se_done * 31u
         + (uint32_t)se_failed * 17u
         + (uint32_t)se_nstocks * 3u
         + (uint32_t)se_last_secs;
}

/* A cheap checksum over the finished rows, logged so a frame off the camera
 * can be compared with one produced by tools/previewcheck on the desktop.
 * The probe uses it on BMP rows for the same reason: to say whether a byte
 * changed without having to look at the panel. */
#define FNV_INIT 2166136261u
static uint32_t se_hash_bytes(uint32_t h, const void *p, int n)
{
    const uint8_t *b = (const uint8_t *)p;
    int i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/*
 * Every distinct event code the hook has seen, and how often, split by
 * whether the menu was on screen at the time.
 *
 * Twenty-four slots is more than the body has buttons; anything past that
 * is counted in the overflow rather than dropped silently, so the line can
 * never claim to be the whole story when it is not.
 */
#define SE_KEY_SLOTS 24
static volatile int se_k_param[SE_KEY_SLOTS];
static volatile int se_k_menu[SE_KEY_SLOTS];
static volatile int se_k_bare[SE_KEY_SLOTS];
static volatile int se_k_n;
static volatile int se_k_other;
static volatile int se_k_total;

/*
 * The last few events in full: all four words and the tick they arrived on.
 *
 * The counts below say which params were seen; this says what an event
 * actually looks like and WHEN the last one was. Both questions are open.
 * The camera reported no button code in the 0x00..0x16 range for a session
 * in which DELETE, PLAY and MENU were all pressed -- so either those
 * presses do not reach this hook, or dispatch had already stopped, and a
 * timestamp on the last event tells those two apart.
 */
#define SE_KEY_TRACE 12
static volatile uint32_t se_kt[SE_KEY_TRACE][5];   /* w0..w3, tick */
static volatile int      se_kt_n;

static void se_key_trace(const int *ev)
{
    int i = se_kt_n;
    if (i >= SE_KEY_TRACE) {
        /* keep the last ones: shuffle down by one */
        int k, c;
        for (k = 0; k < SE_KEY_TRACE - 1; k++)
            for (c = 0; c < 5; c++) se_kt[k][c] = se_kt[k + 1][c];
        i = SE_KEY_TRACE - 1;
    } else {
        se_kt_n = i + 1;
    }
    se_kt[i][0] = (uint32_t)ev[0];
    se_kt[i][1] = (uint32_t)ev[1];
    se_kt[i][2] = (uint32_t)ev[2];
    se_kt[i][3] = (uint32_t)ev[3];
    se_kt[i][4] = (uint32_t)CAM_CALL0(CAM_tickGet, long);
}

static void se_key_count(int param, int menu_up)
{
    int i;
    for (i = 0; i < se_k_n && i < SE_KEY_SLOTS; i++) {
        if (se_k_param[i] == param) {
            if (menu_up) se_k_menu[i]++; else se_k_bare[i]++;
            se_k_total++;
            return;
        }
    }
    if (se_k_n >= SE_KEY_SLOTS) { se_k_other++; se_k_total++; return; }
    i = se_k_n;
    se_k_param[i] = param;
    se_k_menu[i]  = menu_up ? 1 : 0;
    se_k_bare[i]  = menu_up ? 0 : 1;
    se_k_n = i + 1;
    se_k_total++;
}

/*
 * One line per report, written by the watcher and only when something
 * changed. "0F m12 b3" is event 0x0F, twelve times with the menu up and
 * three with it down.
 */
static int se_key_trace_on(void);   /* defined with the other ML/SE.* switches */

static void se_key_report(void)
{
    static int last = -1;
    char line[200];
    int i, k = 0;

    if (se_k_total == last) return;
    last = se_k_total;

    /* se_snprintf returns nothing, so the length is counted here. */
    for (i = 0; i < se_k_n && i < SE_KEY_SLOTS && k < (int)sizeof(line) - 24; i++) {
        se_snprintf(line + k, sizeof(line) - (size_t)k, "%02X m%d b%d  ",
                    (unsigned)se_k_param[i], se_k_menu[i], se_k_bare[i]);
        while (line[k]) k++;
    }
    line[k] = 0;
    se_note("keys (%d seen, %d over, %d trash): %s",
            se_k_total, se_k_other, se_trash_seen, line);

    /*
     * The last dozen events in full -- but only when asked for.
     *
     * This was unconditional, and it is what froze the camera.
     *
     * se_note rewrites the ENTIRE log buffer to the card on every call,
     * so once the log reaches its 16 KB cap every line costs 16 KB. These
     * twelve lines plus the summary and the timestamp are fourteen calls,
     * so one button press wrote about 227 KB to the card -- and a session
     * of ordinary use filled the log, after which every further press did
     * it again. A develop starting in the middle of that, wanting the
     * card for a 12 MB read and a JPEG write, stopped responding.
     *
     * A log came back 69% `ev` lines: 209 of roughly 300. They answered
     * the question they were added for -- which codes this body sends --
     * years of trips ago, and FIRMWARE.md records the answer.
     *
     * So they are opt-in now. Drop an empty ML/SE.KEYS on the card when a
     * button needs identifying again.
     */
    if (!se_key_trace_on()) return;
    for (i = 0; i < se_kt_n && i < SE_KEY_TRACE; i++)
        se_note("  ev %08X %08X %08X %08X  t=%u",
                (unsigned)se_kt[i][0], (unsigned)se_kt[i][1],
                (unsigned)se_kt[i][2], (unsigned)se_kt[i][3],
                (unsigned)se_kt[i][4]);
    se_note("  now t=%u", (unsigned)CAM_CALL0(CAM_tickGet, long));
}

/*
 * ML/SE.NOUI: develop frames, draw nothing, claim no buttons.
 *
 * The escape hatch that does not need the camera. Deleting SE.BIN already
 * disables everything, but that loses the develop with it; this keeps the
 * part that works and switches off the part that can reach the display.
 */
static int se_no_ui(void)
{
    static int known = -1;
    uint32_t sz = 0;
    if (known >= 0) return known;
    CAM_CALL2(CAM_FIO_GetFileSize, int, "A:/ML/SE.NOUI", &sz);
    known = sz ? 1 : 0;
    return known;
}

/* ML/SE.KEYS: dump the raw button events again. Off by default -- see
 * the long note in se_key_report. */
static int se_key_trace_on(void)
{
    static int known = -1;
    uint32_t sz = 0;
    if (known >= 0) return known;
    CAM_CALL2(CAM_FIO_GetFileSize, int, "A:/ML/SE.KEYS", &sz);
    known = sz ? 1 : 0;
    return known;
}

static int se_hold_develop(void)
{
    static int known = -1;
    uint32_t sz = 0;
    if (known >= 0) return known;
    CAM_CALL2(CAM_FIO_GetFileSize, int, "A:/ML/SE.HOLD", &sz);
    known = sz ? 1 : 0;
    return known;
}


/* ------------------------ the imaging-buffer probes, and why they are gone */
/*
 * Removed: the develop-seam reconnaissance. Three hundred and forty lines
 * of it -- se_dev_sample, se_dev_report, se_snap_poll, se_snap_write,
 * se_dump_one and se_dump_buffers.
 *
 * It was built to answer one question: which of the three pointers in
 * Canon's imaging state object is the buffer DIGIC fills before JpCore
 * encodes it. That question is answered and no longer load-bearing -- the
 * camera develops from the embedded preview or from the raw spool, and
 * neither path goes anywhere near those buffers.
 *
 * It had to go because it made the camera show Err when the shutter was
 * pressed. se_snap_poll read 256 bytes from each of the three buffers
 * through the UNCACHED alias, and the watcher's idle loop ran it a hundred
 * times a second, forever -- so it was reading imaging memory straight
 * through every capture, and when the content changed, which is exactly
 * what a capture does, it copied 65,536 more bytes out of a buffer DIGIC
 * was in the middle of writing.
 *
 * Read-only was never the safety property that mattered. An uncached burst
 * read contending with DIGIC's own DMA on the same memory is interference
 * whether or not a byte is written back.
 *
 * The rule this broke was already written down -- "never touch EDMAC
 * registers during capture", and the file header says the watcher polls
 * the directory precisely so that nothing of ours is near the capture
 * path. It was a comment, so the code drifted away from it over the trips
 * that needed the probe. tools/camsim asserts it now: the simulated
 * imaging buffers count every read, and a capture with a non-zero count
 * fails the suite.
 */

/*
 * The sixteen words the DIGIC palette registers get, and they are ours.
 *
 * Format, measured at ConvertYUVPaletteToPBPalette (0xFFB0A528):
 *
 *     word = (opacity << 24) | (Y << 16) | (U << 8) | V
 *
 * opacity 3 is opaque; an alpha byte of 0x3F or less converts to
 * 0x00FC0000, the transparent entry (0xFFB0A568). U and V are signed,
 * zero neutral, BT.601. Checked against four of ML's own entries -- see
 * the long note in engine/ui.h, which also has the RGB each word came
 * from and keeps tools/fb2png.py honest.
 *
 * ENTRY 15 IS NOT A DESIGN CHOICE. set_ml_palette_if_dirty (0x00981C50)
 * decides whether to overwrite the palette by comparing exactly one word,
 * PB_Palette[47] at 0x0002943C -- the shadow register for entry 15 --
 * against 0x03EB0000 (`ldr r5, =0x03EB0000` at 0x00981C68, `cmp r3, r5`
 * at 0x00981C70). Holding entry 15 at that value, and writing it into the
 * shadow as ML does at 0x00981C80, means ML's own test is permanently
 * satisfied and ML never puts its palette back over ours. Change it and
 * every ML draw anywhere in the image will fight us for the registers.
 *
 * Not static: tools/camsim checks the structure of this table on every run
 * -- entry 0 transparent, entry 15 exactly ML's marker, everything else
 * opaque -- because a private copy of a table is precisely how a wrong
 * number gets confirmed instead of caught, which is what the Debug entry
 * layout did for two camera trips.
 */
const uint32_t se_palette[16] = {
    0x00FC0000u,   /* 0  CLEAR           transparent -- ML's own value    */
    0x03170003u,   /* 1  INK    #1E1A17  near-black, warm                 */
    0x0332FE06u,   /* 2  SHADOW #3D362F  deep warm grey                   */
    0x034FFD08u,   /* 3  SLATE  #5E554A  warm grey, dark                  */
    0x0371FD0Bu,   /* 4  MID    #857A6B  warm grey                        */
    0x0394FF0Du,   /* 5  EDGE   #ABA092  warm grey, light                 */
    0x03C7FE0Fu,   /* 6  PAPER  #E2D9C4  off-white -- the ground          */
    0x03D8000Eu,   /* 7  HILITE #F2EBDA  paper highlight                  */
    0x0348EE25u,   /* 8  RUST   #8A3B22  deep rust                        */
    0x0361E836u,   /* 9  RED    #C04A2E  brick red                        */
    0x038CD72Du,   /* A  AMBER  #DC8A32  amber                            */
    0x03A9D51Bu,   /* B  GOLD   #D9BE4A  mustard gold                     */
    0x036FF1FDu,   /* C  OLIVE  #6B8A4E  olive green                      */
    0x035911EBu,   /* D  TEAL   #357380  teal                             */
    0x0360071Au,   /* E  PLUM   #8E5670  dusty plum                       */
    0x03EB0000u    /* F  WHITE           FIXED -- ML's marker, see below  */
};

/*
 * Is the palette in the registers still ours? (C2)
 *
 * OUR shadow word, not ML's, and not the registers.
 *
 * Two wrong answers already, so the reasoning is written out in full.
 *
 * It was ML's test, transcribed: `MEM(0x0002943C) == 0x03EB0000`, which is
 * what set_ml_palette_if_dirty compares at 0x00981C6C. That is right for
 * ML and wrong for us, because entry 15 is pinned to ML's own value so ML
 * stays quiet -- pin that word and ML's question and ours become the same
 * question. It answered "already ours" while the hardware held ML's
 * palette, our sixteen words were never written, and the interface came up
 * red: our INK is index 1, and index 1 in ML's palette is red. `pal=0`.
 *
 * Then it read the sixteen REGISTERS, which is worse. Nothing in the
 * firmware ever reads them: across the whole 8 MB ROM
 * 0xC0F14080..0xC0F140BC appear as literals in exactly one place, the
 * descriptor table at 0xFFB6AF50 that SetParameterToBitmapDisplayDevice
 * uses only to write. Canon's own read-back, GetYUVPaletteFromDisplay-
 * Device (0xFFB0BC28), is memcpy(dst, 0x00029450, 64) -- RAM. ML's test
 * reads RAM. Three parties who all wanted this information, none of whom
 * reads the register. They are write-only as far as this firmware is
 * concerned, and I put sixteen reads of them on the path between the
 * button and the screen.
 *
 * So: a shadow word in RAM that ML does not use. See CAM_PB_PAL_MINE.
 */
/*
 * fake_simple_button(BGMT_MENU), transcribed.
 *
 * ML's own is at 0x00988418 and its FIRST instruction is the shutdown
 * guard -- `ldr r3, =0x009C84E0; ldr r1,[r3]; cmp r1,#0; bxne lr` -- so
 * the guard covers every call site rather than each caller remembering.
 * Ours had it on the open path and not the close path, which is exactly
 * the sort of asymmetry a helper removes.
 *
 *   0x00988428  ldr r2, =0xFFFE1DC0      FAKE_BTN
 *   0x0098842C  mov r3, r1               0
 *   0x00988430  b   0xFF8190D0           GUI_Control(code, 0, FAKE_BTN, 0)
 */
static void se_fake_menu(void)
{
    if (se_shutting_down()) return;
    CAM_CALL4(CAM_GUI_Control, void, SE_BGMT_MENU, 0, SE_FAKE_BTN, 0);
}

static int se_palette_is_ours(void)
{
    return MEM(CAM_PB_PAL_MINE) == se_palette[SE_PAL_MINE_INDEX];
}

/*
 * Canon's own sixteen palette words, captured once, before we overwrite
 * them for the first time.
 *
 * docs/ML_DISPLAY.md 12.1 is the one unknown that could still make the
 * picker invisible after D1 and D2 are fixed, and it is unresolvable from
 * the ROM: SelectParameter (0xFFB0A95C) builds the palette from Canon's
 * YUV table at MEM(0x00029450), which is RAM with no initialiser in the
 * image. Entries whose alpha byte is <= 0x3F convert to 0x00FC0000 --
 * transparent (0xFFB0A568) -- so if a lot of Canon's entries are
 * transparent then a frame drawn under them is a frame nobody sees, and
 * that is exactly the symptom this project keeps getting.
 *
 * Reading sixteen MMIO words costs nothing and changes nothing, and it
 * means that if the picker is still invisible the log says why instead of
 * the next session guessing. This is a diagnostic, not a second change.
 */
static volatile uint32_t se_canon_pal[16];
static volatile int      se_canon_pal_have;

static void se_set_palette(void)
{
    int i;

    if (!MEM(CAM_display_is_on)) return;

    if (!se_canon_pal_have) {
        for (i = 0; i < 16; i++)
            se_canon_pal[i] = (uint32_t)MEM(CAM_PB_PAL_VAL(i));
        se_canon_pal_have = 1;
    }
    /*
     * DIGIC 4 keeps the palette in LCD_Palette: word 0 of each triple is
     * the register EngDrvOut writes, word 2 is the shadow a later flush
     * will put back. Both have to move, or the next flush restores Canon
     * and the frame is drawn in the wrong colours. Entry 0 stays the
     * transparent word; the other fifteen are ours.
     */
    for (i = 0; i < 16; i++) {
        uintptr_t reg = (uintptr_t)MEM(CAM_LCD_PAL_REG(i));
        uintptr_t reg2 = (uintptr_t)MEM(CAM_LCD_PAL_REG2(i));
        MEM(CAM_PB_PAL_VAL(i)) = se_palette[i];
        if (reg)  CAM_CALL2(CAM_EngDrvOut, void, reg, se_palette[i]);
        if (reg2) CAM_CALL2(CAM_EngDrvOut, void, reg2, se_palette[i]);
    }
}

/* ===================================================================== */
/* The screen                                                             */
/* ===================================================================== */
/*
 * The soup engine's own interface, on the panel, with nothing of Magic
 * Lantern between it and the hardware.
 *
 * engine/ui.c renders the whole thing into a 360-byte x 240-row buffer of
 * 4-bit pixels, which is exactly the hardware's format. Putting it on
 * screen is one memcpy into the overlay buffer at MEM(0x00029328) -- the
 * same destination and the same 86,400 bytes that ML's bmp_idle_copy
 * writes on every menu redraw, checked before anything is written through
 * it.
 *
 * This replaces the arrangement where our frame was drawn from inside ML's
 * menu redraw, as a display callback installed into all 74 of its entries.
 * That worked, and it meant ML drew its own tab bar and header around
 * ours, the wheel cycled ML's tabs underneath, and the menu flashed up
 * before the film selector did. None of that is fixable from inside
 * somebody else's redraw.
 *
 * The staging buffer is allocated once and kept: 84 KB against the 2264 KB
 * the body reports free, and allocating it per frame would churn a heap
 * whose fragmentation has already cost this project a develop.
 */

/*
 * How often the screen task wakes, and how many wakes pass before it gives
 * up and closes. (C6)
 *
 * 100 ms, and a paint on EVERY tick. This used to be 250 ms with a repaint
 * only every fourth wake unless the frame had changed -- about one paint a
 * second -- and that is not fast enough, for a reason the Phase 1 work made
 * plain: Canon's own menu is underneath ours. Canon redraws it on its own
 * schedule, and nothing in its GUI widget code asks our permission. At one
 * paint a second an asynchronous Canon repaint leaves Canon's menu on the
 * panel for up to a second.
 *
 * Magic Lantern never has this problem and never needed a timer: its
 * menu_redraw_task (0x00986C1C) blocks on a message queue, repaints on
 * every event that posts to it, and sleeps 20 ms between repaints
 * (0x00986C84) -- so while anything is happening it redraws at up to
 * 50 Hz, and while nothing is happening nothing else is drawing either.
 * We have one trigger and a live Canon dialog underneath, so we paint.
 *
 * The cost is bounded and small: the render is skipped when the frame has
 * not changed (the signature test below), so a tick is 240 memcpys of 360
 * bytes and nothing else.
 *
 * SE_SCR_IDLE is in ticks, so it has to be rescaled with them to keep the
 * same forty seconds of inactivity before the picker closes itself: 400
 * ticks of 100 ms, not the 160 of 250 ms it was. Scaling the tick without
 * it would have left the panel lit for nearly seven minutes.
 */
#define SE_SCR_TICK_MS  100
#define SE_SCR_REFRESH  1
#define SE_SCR_IDLE     400

/*
 * How long the closed screen waits between looks at se_scr_want.
 *
 * This is the whole of the button-to-picker latency now that the card
 * write is gone, so it is not the paint tick: 20 ms, which is a flag read
 * and a sleep, against 100 ms of doing nothing useful.
 */
#define SE_SCR_IDLE_MS  20

/* Not static: the simulator opens and closes the screen directly, which
 * is what ML/SE.UI does on the camera -- the buttons are a separate
 * question and are being measured rather than guessed at. */
volatile int se_scr_want;               /* the user asked for the engine */
static volatile int se_scr_poke;        /* bumped by every button press  */
static volatile int se_scr_pinned;      /* ML/SE.UI: stay open            */
static volatile int se_scr_was_shown;   /* the layer's state before we ran */
/* Not static: the simulator waits for the screen to actually shut before
 * resetting the panel underneath it. */
volatile int se_scr_open;               /* ...and it is on the panel     */
/* Not static: the simulator reads it to tell an open screen from a closed
 * one without having to parse the log. */
volatile int se_scr_blits;
static volatile int se_scr_badptr, se_scr_nomem, se_scr_dark;
static volatile int se_scr_palettes;    /* times Canon's palette was replaced */

/*
 * The open path's timings, recorded here and logged by the watcher.
 *
 * NOT logged as they happen. se_note creates, rewrites and closes the whole
 * 16 KB log file on every single call -- look at the end of it -- and the
 * screen task used to call it BEFORE GUI_Control:
 *
 *     se_note("screen: asking Canon for the panel (fake MENU)");
 *     CAM_CALL4(CAM_GUI_Control, ...);
 *
 * so pressing DELETE paid a synchronous CompactFlash create+write+close,
 * queued behind whatever the watcher's directory walk was doing on the same
 * card, before Canon was even asked. The camera measured the cost: TRASH
 * arrived at t=817 and our own fake MENU reached the hook at t=1163 --
 * 3.46 seconds -- while Canon itself took 50 ms ("screen: panel up after
 * 50 ms"). The delay was entirely ours.
 *
 * This is the same rule as se_key's, one task over: nothing between a
 * button and the thing it does may touch the card.
 */
static volatile uint32_t se_scr_t_ask;   /* tick the button was noticed  */
static volatile uint32_t se_scr_t_up;    /* tick the panel was up        */
static volatile uint32_t se_scr_t_open;  /* tick the first frame went out */
static volatile int      se_scr_tries;   /* poll iterations it took      */
static volatile int      se_scr_timing;  /* 1 = a new set to report      */

/*
 * How far the screen task got, so that "the menu did not open" is a
 * diagnosis instead of a question.
 *
 * This is the third attempt at the open path and the second time the
 * symptom has been "nothing happened" -- the least informative thing a
 * camera can report. Every step records where it is, the watcher prints
 * the high-water mark, and a screen that gets stuck now says in which
 * call.
 */
#define SE_SCRP_IDLE      0   /* nothing asked for                        */
#define SE_SCRP_WANT      1   /* saw se_scr_want, no frame buffer yet     */
#define SE_SCRP_READY     2   /* buffer exists, about to look at the panel */
#define SE_SCRP_ASKED     3   /* GUI_Control returned                     */
#define SE_SCRP_PANEL     4   /* the poll finished                        */
#define SE_SCRP_PAINTED   5   /* the first frame went out                 */
#define SE_SCRP_OPEN      6   /* steady state                             */
static volatile int      se_scr_phase;
static volatile int      se_scr_phase_max;

static void se_scr_at(int phase)
{
    se_scr_phase = phase;
    if (phase > se_scr_phase_max) se_scr_phase_max = phase;
}
static volatile uintptr_t se_scr_buf;

/*
 * Saving and restoring Canon's palette is deliberately gone.
 *
 * ML's restore_canon_palette() is an empty function whose entire body is
 * the comment "unsafe", with the real call left commented out beside it.
 * We were writing sixteen DIGIC registers back on every close, which is
 * the thing ML looked at and decided not to do.
 */

/* One frame onto the panel. Returns 0 if it could not be drawn. */
static int se_paint(void)
{
    volatile uint8_t *hw;
    uintptr_t p;

    /*
     * Both tests, because Magic Lantern has both. (C4)
     *
     * This used to note MEM(0x0000FCA0) and carry on, on the reasoning that
     * the pointer is the honest signal. The pointer IS good evidence and it
     * is not sufficient -- five separate places in ML's own path refuse to
     * do anything while that word is clear:
     *
     *   menu_redraw_do           0x0098668C   beq, draws nothing at all
     *   set_ml_palette           0x00981BD4
     *   set_ml_palette_if_dirty  0x00981C60
     *   EngDrvOut                0x00989134
     *   ShowBitmap               0xFFB09820
     *
     * and there is a measured reason the flag can lie the other way too:
     * TurnOnDisplay sets it to 1 at 0xFFB09210, at the TOP of the function,
     * before TurnOnPowerPort and before the callbacks that program the
     * palette. So the flag can be true while the bring-up is still
     * running. Requiring both narrows that window to nothing we can
     * observe.
     */
    if (!MEM(CAM_display_is_on)) { se_scr_dark++; return 0; }

    /*
     * And Canon's bitmap screen has to actually exist.
     *
     * MEM(0x0000F3A8) is the latch gui_create_bitmap_screen sets at
     * 0xFFA25300 once CreatePhysicalVram and DisplayPhysicalScreen have
     * both run, and that the teardown at 0xFFA279B0 clears at 0xFFA279F4
     * BEFORE calling DeletePhysicalVram (0xFFB0B0E4) to free the buffer.
     *
     * Without this we blit 86,400 bytes into that buffer on either side of
     * its life: during Canon's boot before it exists, and during shutdown
     * after it has been freed. The second is a write into released heap,
     * ten times a second, while the camera is trying to power down.
     */
    if (se_shutting_down()) {
        se_scr_dark++;
        return 0;
    }

    p = cam_overlay_ptr();
    se_scr_buf = p;
    if (!cam_panel_ptr_ok(p)) { se_scr_badptr++; return 0; }
    if (!se_fb) { se_scr_nomem++; return 0; }
    hw = (volatile uint8_t *)p;

    /*
     * The palette, before every frame that is actually going out. (C2)
     *
     * Canon rebuilds its own sixteen words whenever the panel is turned
     * on: TurnOnDisplay walks the callback list at MEM(0x00029318) with
     * r0 = 0 (0xFFB09280), one of those callbacks is
     * NotifyTurnOnDisplayToBitmapDisplayDevice (0xFFB0AC74), and it calls
     * SelectParameter (0xFFB0A95C) -> rebuild all sixteen shadows from
     * Canon's YUV table at MEM(0x00029450) -> SetParameterToBitmapDisplay-
     * Device (0xFFB0A888) -> flush them to 0xC0F14080..BC. Entries whose
     * alpha byte is <= 0x3F come out of ConvertYUVPaletteToPBPalette as
     * 0x00FC0000, which is transparent (0xFFB0A568).
     *
     * So a frame blitted with Canon's palette in place is invisible, or
     * near enough, and nothing says so. ML checks on EVERY single draw --
     * bmp_vram's first instruction is a call to set_ml_palette_if_dirty
     * (0x00980BC0) -- and tail-calls set_ml_palette again at the end of
     * every redraw (0x00986BC4, a `b` not a `bl`). The previous build
     * wrote it once, at open, guarded by a DISPLAY_IS_ON that the log
     * shows was zero both times, so it was never written at all.
     */
    if (!se_palette_is_ours()) { se_set_palette(); se_scr_palettes++; }

    /*
     * Render only when the frame would differ. The blit still happens
     * every pass -- Canon repaints the overlay for its own reasons and our
     * frame has to win -- but drawing a screenful of glyphs ten times a
     * second to produce the same bytes is work for nothing, on a core that
     * is usually developing a photograph at the same time.
     */
    {
        static uint32_t last_sig;
        static int      have;
        uint32_t sig = se_ui_sig();
        if (!have || sig != last_sig || se_ui_dirty) {
            se_render();
            last_sig = sig;
            have = 1;
            se_ui_dirty = 0;
        }
    }
    /* No logging here. se_paint runs on the open path and ten times a
     * second after it; se_note is a whole-file card write. The buffer
     * address is in the watcher's report instead. */
    /*
     * Row by row, which is what bmp_idle_copy(1, 0) does. (C5)
     *
     * 0x00980B58: r6 = real + 0x15180; r5 = idle - real; then
     *   0x00980B64  mov r0, r4            ; dst = real + 360i
     *   0x00980B68  add r1, r5, r4        ; src = idle + 360i
     *   0x00980B6C  mov r2, #360
     *   0x00980B70  add r4, r4, #360
     *   0x00980B74  bl  0xFFB44664        ; Canon's memcpy
     *   0x00980B78  cmp r4, r6
     *   0x00980B7C  bne 0x00980B64
     *
     * and menu_redraw_do takes that branch, not the single-memcpy one:
     * at 0x00986B84 it loads r1 from [r3,#0x128] and branches to
     * 0x00986BA4 only when that tested zero, so `full` is 0 at the call.
     *
     * 240 x 360 is the same 86,400 bytes as one memcpy and, as far as can
     * be told from here, has the same effect. It is transcribed rather
     * than simplified because "the same effect as far as can be told from
     * here" is what the lazy-write bug looked like too.
     */
    /*
     * The UI buffer is 720x240, two 4-bit pixels per byte, drawn for a
     * panel whose pixels are square. The 50D overlay is 720x480 inside a
     * 960-byte stride, one byte per pixel. Each source row is written
     * twice and each nibble becomes its own byte. Row 0 is a single
     * memcpy of pitch bytes so the simulator's first-row checks still
     * see one frame.
     */
    {
        int y, x;
        uint8_t line[ML_BMP_PITCH];

        for (y = 0; y < SE_UI_H; y++) {
            const uint8_t *src = se_fb + (unsigned)y * SE_UI_PITCH;
            for (x = 0; x < SE_UI_W; x++) {
                uint8_t b = src[x >> 1];
                line[x] = (x & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0F);
            }
            for (; x < ML_BMP_PITCH; x++) line[x] = 0;
            cam_memcpy(hw + (unsigned)(y * 2) * ML_BMP_PITCH, line, ML_BMP_PITCH);
            cam_memcpy(hw + (unsigned)(y * 2 + 1) * ML_BMP_PITCH, line, ML_BMP_PITCH);
        }
    }
    se_scr_blits++;
    return 1;
}

/*
 * The open path's timings, from the watcher.
 *
 * Ticks are 10 ms (tickGet at 0xFFB58DE4; the port calls
 * sysClockRateSet_100). Three numbers say where the time went:
 *
 *   ask->up    Canon lighting the panel. It was 50 ms on the camera.
 *   up->open   our first frame: render plus 240 memcpys of 360 bytes.
 *   total      what the user actually waits after pressing DELETE.
 *
 * If total is ever seconds again, these say which third of the path owns
 * it instead of leaving it to be guessed at.
 */
static void se_scr_timing_report(void)
{
    uint32_t a, u, o;
    if (!se_scr_timing) return;
    se_scr_timing = 0;
    a = se_scr_t_ask; u = se_scr_t_up; o = se_scr_t_open;
    if (!a) return;
    se_note("screen: open took %u ms (canon %u, first frame %u), "
            "polls=%d blits=%d",
            (unsigned)(o - a) * 10u, (unsigned)(u - a) * 10u,
            (unsigned)(o - u) * 10u, se_scr_tries, se_scr_blits);
}

/*
 * Canon's palette, once, from the watcher -- never from the screen task,
 * which must not touch the card.
 */
static void se_canon_pal_report(void)
{
    static int done;
    if (done || !se_canon_pal_have) return;
    done = 1;
    se_note("canon palette (before ours, from PB_Palette 0x00029380):");
    se_note("  %08X %08X %08X %08X %08X %08X %08X %08X",
            (unsigned)se_canon_pal[0], (unsigned)se_canon_pal[1],
            (unsigned)se_canon_pal[2], (unsigned)se_canon_pal[3],
            (unsigned)se_canon_pal[4], (unsigned)se_canon_pal[5],
            (unsigned)se_canon_pal[6], (unsigned)se_canon_pal[7]);
    se_note("  %08X %08X %08X %08X %08X %08X %08X %08X",
            (unsigned)se_canon_pal[8], (unsigned)se_canon_pal[9],
            (unsigned)se_canon_pal[10], (unsigned)se_canon_pal[11],
            (unsigned)se_canon_pal[12], (unsigned)se_canon_pal[13],
            (unsigned)se_canon_pal[14], (unsigned)se_canon_pal[15]);
    /*
     * The top byte is the opacity field: 3 is fully opaque and 0x00FC0000
     * is the transparent entry ConvertYUVPaletteToPBPalette returns for an
     * alpha byte of 0x3F or less (0xFFB0A528..0xFFB0A5C4). So counting the
     * entries whose top byte is zero says, in one number, how much of our
     * frame would have been invisible under Canon's palette.
     */
    {
        int i, clear = 0;
        for (i = 0; i < 16; i++)
            if ((se_canon_pal[i] >> 24) == 0) clear++;
        se_note("  %d of 16 were transparent (top byte 0)", clear);
    }
}

static void se_screen_report(void)
{
    /*
     * The phase is reported even when the blit count has not moved. A
     * screen that never opened never blits, so the old "only when blits
     * change" rule meant the one failure worth diagnosing was the one that
     * printed nothing at all.
     */
    {
        static int last_phase = -1;
        if (se_scr_phase_max != last_phase) {
            last_phase = se_scr_phase_max;
            se_note("screen: reached phase %d "
                    "(1 want 2 ready 3 asked 4 panel 5 painted 6 open)",
                    se_scr_phase_max);
        }
    }

    /*
     * One line when something DIAGNOSTIC changes -- and not one per blit.
     *
     * This used to report whenever se_scr_blits moved, which was fine at
     * one paint a second and is not fine at ten: the log is a 16 KB rolling
     * buffer, and a run of "screen: blits=N" lines pushes out the lines
     * that are actually worth reading. The desktop suite caught it by
     * failing on a missing `look hash` -- the develop's own verification
     * had been rolled off the end. On the camera that would have cost a
     * trip and left the one channel that survives a task dying full of
     * nothing but a counter.
     *
     * So: the counters that mean something is wrong, plus the buffer
     * address, and the blit count only when one of those moves or every
     * 256th frame as a heartbeat.
     */
    static int last_sig = -1, last_blits = -1;
    int sig = se_scr_dark + se_scr_badptr * 1000 + se_scr_nomem * 1000000
            + se_scr_palettes * 17;
    int beat = (se_scr_blits & ~0xFF);

    if (sig == last_sig && beat == last_blits) return;
    last_sig = sig;
    last_blits = beat;
    se_note("screen: blits=%d dark=%d badptr=%d nomem=%d pal=%d "
            "phase=%d/%d vram=0x%08X",
            se_scr_blits, se_scr_dark, se_scr_badptr, se_scr_nomem,
            se_scr_palettes, se_scr_phase, se_scr_phase_max,
            (unsigned)se_scr_buf);
}

/*
 * The screen task.
 *
 * Its own task rather than part of the watcher, because the watcher spends
 * eighteen seconds at a time developing a frame and the interface has to
 * stay alive and responsive while it does -- the progress bar is the whole
 * point of having one.
 *
 * It paints at about ten frames a second while open and does nothing at
 * all while closed, which is most of the time: nothing is drawn, the
 * palette is Canon's, and the camera has no idea we exist.
 */
void se_screen(void)
{
    int idle = 0;
    int since = 0;

    for (;;) {
        /*
         * Stop, the way every one of ML's nine tasks does -- its
         * menu_redraw_task is `while (!MEM(0x009C84E0))` at 0x00986C8C.
         * Nothing of ours may still be painting into Canon's overlay while
         * Canon is tearing the display down.
         */
        if (se_shutting_down()) {
            se_scr_open = 0;
            se_scr_at(SE_SCRP_IDLE);
            cam_msleep(SE_SCR_IDLE_MS);
            continue;
        }

        /*
         * Nothing opens until there is something to draw.
         *
         * The camera reported "nomem=7": seven paint attempts with se_fb
         * still NULL, because the button was pressed before the watcher
         * had allocated it. That is the long delay before the interface
         * appeared -- it was open and empty the whole time.
         */
        if (se_scr_want && !se_scr_open && !se_fb) {
            se_scr_at(SE_SCRP_WANT);
            cam_msleep(SE_SCR_IDLE_MS);
            continue;
        }

        if (se_scr_want && !se_scr_open) {
            /*
             * Ask Canon for the screen. Do not take it.
             *
             * This is Magic Lantern's entire display strategy on this
             * body, and it has worked for a decade:
             *
             *     if (!DISPLAY_IS_ON) fake_simple_button(BGMT_MENU);
             *     msleep(50);
             *
             * Canon opens its own menu, which powers the panel, shows the
             * bitmap layer and sets the GUI state, in the order Canon
             * intends. ML then draws over the top. Its piggyback and
             * front-buffer calls are both compiled out for CONFIG_5DC, so
             * that really is all of it.
             *
             * The build before this one did it the other way -- called
             * TurnOnDisplay and ShowBitmap directly, which the ROM says
             * are the right calls and which did put the interface on
             * screen. Then the camera froze. Being right about what a
             * function does is not the same as being right to call it.
             */
            se_scr_at(SE_SCRP_READY);
            se_scr_t_ask = (uint32_t)CAM_CALL0(CAM_tickGet, long);
            if (!MEM(CAM_display_is_on)) {
                int tries;
                /*
                 * Ask Canon FIRST. No logging on this path -- see the note
                 * on se_scr_t_ask. The ticks are recorded and the watcher
                 * prints them a moment later.
                 */
                if (se_shutting_down()) { se_scr_want = 0; continue; }
                se_fake_menu();
                se_scr_at(SE_SCRP_ASKED);
                /*
                 * Wait for it. (C3)
                 *
                 * ML does not: menu_open fakes the press at 0x0098728C,
                 * sleeps 50 ms and goes straight on to setting menu_shown
                 * and posting a redraw. It gets away with it because
                 * menu_redraw_do simply returns while the panel is dark
                 * (0x0098668C) and the next key posts another redraw.
                 *
                 * We have one trigger, and the camera measured that 50 ms
                 * is not enough -- "screen: displayon=0 vram=0x105890C0"
                 * was logged after exactly that wait. So poll the
                 * precondition rather than assume it. Both halves of it:
                 * TurnOnDisplay sets DISPLAY_IS_ON at 0xFFB09210, at the
                 * top, before the power port and before the palette
                 * callbacks, so the flag alone can be true too early.
                 */
                /*
                 * Check before sleeping, and in small steps.
                 *
                 * The old loop slept 50 ms before its first look, so even
                 * an instant panel cost 50 ms, and the step was 50 ms.
                 * Canon came up in 50 ms on the camera, so the step is
                 * what the response time now is: 10 ms, 200 of them, same
                 * 2-second budget.
                 */
                for (tries = 0; tries < 200; tries++) {     /* up to 2 s */
                    if (MEM(CAM_display_is_on) &&
                        cam_panel_ptr_ok(cam_overlay_ptr())) break;
                    /* the user changed their mind: stop waiting for a
                     * panel we are no longer going to draw on */
                    if (!se_scr_want) break;
                    cam_msleep(10);
                }
                se_scr_tries = tries;
            }
            se_scr_at(SE_SCRP_PANEL);
            se_scr_t_up = (uint32_t)CAM_CALL0(CAM_tickGet, long);

            /*
             * No palette write here any more.
             *
             * It belongs in se_paint, immediately before each frame goes
             * out, because Canon puts its own back on every panel-on and
             * once-at-open cannot see that happen. See se_paint.
             *
             * It is still never put back on close. ML's
             * restore_canon_palette is an empty function whose whole body
             * is the comment "unsafe", with the real call left commented
             * out beside it, and we were writing sixteen DIGIC registers
             * on every close.
             */

            se_scr_open = 1;
            idle = since = 0;
            /*
             * Paint immediately rather than waiting for the loop to come
             * round to it. One tick of 100 ms does not sound like much and
             * it is a quarter of the whole budget now that the card write
             * is gone.
             */
            se_paint();
            se_scr_at(SE_SCRP_PAINTED);
            se_scr_t_open = (uint32_t)CAM_CALL0(CAM_tickGet, long);
            se_scr_timing = 1;             /* the watcher will report it */
        } else if (!se_scr_want && se_scr_open) {
            se_scr_open = 0;
            /* Hand the screen back the way ML does: ask Canon to close
             * its menu. No palette restore -- see above. */
            if (MEM(CAM_display_is_on)) {
                se_fake_menu();
                cam_msleep(50);
            }
            se_scr_timing = 1;             /* the watcher reports the close */
        }

        if (!se_scr_open) { idle = 0; cam_msleep(SE_SCR_IDLE_MS); continue; }
        se_scr_at(SE_SCRP_OPEN);

        /*
         * If the panel goes out, the interface is over.
         *
         * ML's menu task does exactly this -- `if (!DISPLAY_IS_ON && ...)
         * gui_stop_menu()` -- and the camera showed why: "dark=2", our
         * screen still open and painting into a panel Canon had powered
         * down underneath us.
         */
        if (!MEM(CAM_display_is_on)) {
            se_note("screen: the panel went out, closing");
            se_scr_want = 0;
            continue;
        }

        {
            static int last_poke;
            if (se_scr_poke != last_poke) { last_poke = se_scr_poke; idle = 0; }
        }

        if (se_ui_dirty || ++since >= SE_SCR_REFRESH) {
            since = 0;
            se_paint();
        }

        if (!se_scr_pinned && ++idle >= SE_SCR_IDLE) {
            se_note("screen: idle, closing itself");
            se_scr_want = 0;
            continue;
        }

        cam_msleep(SE_SCR_TICK_MS);
    }
}

/* ---------------------------------------------------------------- paint */

static const char *const se_res_name[2] = { "HALF", "FULL" };


/* Advance to the next stock, wrapping. */
void se_next_stock(void)
{
    if (!se_nstocks) return;
    if (++se_stock_index >= se_nstocks) se_stock_index = 0;
    se_ui_dirty = 1;
    se_cfg_dirty = 1;
}

void se_prev_stock(void)
{
    if (!se_nstocks) return;
    if (--se_stock_index < 0) se_stock_index = se_nstocks - 1;
    se_ui_dirty = 1;
    se_cfg_dirty = 1;
}

/* Preview JPEG <-> raw sensor data. Read once per frame by develop_one, so
 * this takes effect on the next shot and never mid-develop. */
void se_toggle_source(void)
{
    se_src_index = (se_src_index == SE_SOURCE_RAW)
                 ? SE_SOURCE_PREVIEW : SE_SOURCE_RAW;
    se_ui_dirty = 1;
    se_cfg_dirty = 1;
}

/* ===================================================================== */
/* Develop one frame                                                      */
/* ===================================================================== */
/*
 * The film look, applied to the JPEG Canon already made.
 *
 * Every CR2 carries a full-size baseline JPEG in IFD0 -- 2496x1664 on this
 * body, about 2 MB -- that DIGIC has already demosaiced, white balanced and
 * colour corrected. Decoding that and running the stock's per-channel curves
 * over it is the architecture agreed at the start: the camera does the hard
 * part, we own the tone response.
 *
 * It also deletes everything that kept going wrong. The raw strip is 11 MB
 * of lossless JPEG that took 135 seconds and 50 MB of card traffic through a
 * spool, and never did decode correctly on the body. This reads a fifth as
 * much, holds it all in memory, and writes nothing but the finished file.
 *
 * The raw pipeline is still there, still tested, still byte-exact on the
 * desktop -- it is simply not what the camera runs for now.
 */

/*
 * The finished JPEG is accumulated whole and written in as few calls as
 * possible, rather than streamed out of the encoder's 4 KB staging buffer.
 *
 * That buffer is reused the moment the sink returns, and Canon's
 * FIO_WriteFile returns before the data has left it: the first camera frame
 * that decoded correctly still came out banded, and diffing it against the
 * desktop showed 318 wrong bytes in twelve runs, every one of them starting
 * 4068 to 4077 bytes into a 4096-byte block. The tail of each write was
 * being overwritten while the card was still reading it. The same signature
 * appeared years ago in Magic Lantern's own ROM dumper, whose staging buffer
 * corrupted bytes near each chunk boundary.
 *
 * Buffering the whole file means the usual frame is written exactly once,
 * with nothing touching the buffer afterwards. A frame too large for the
 * buffer flushes early, and those flushes are followed by a short pause --
 * a few tens of milliseconds across a whole frame, against the seconds the
 * develop already takes.
 */
#define SE_OUTHALF (24 * 1024)
#define SE_OUTBUF  (SE_OUTHALF * 2)

typedef struct {
    int      h;
    int      bytes;       /* total written to the file          */
    uint8_t *buf[2];      /* alternated, so a write is never    */
    int      cur;         /* ...reading a buffer we are filling */
    int      len;
    int      err;
} jpeg_out;

/*
 * Two buffers, used alternately. Writing from one while filling the other
 * means the card is never reading a buffer that is being refilled: the
 * previous write has a whole buffer's worth of encoding to finish in, and
 * nothing touches it until the write after next. A single buffer is only
 * safe when the whole file fits in it, which a larger frame will not.
 */
static int jpeg_flush(jpeg_out *o)
{
    int n = o->len;
    if (!n) return 0;
    o->len = 0;
    if (cam_write(o->h, o->buf[o->cur], (size_t)n) != n) {
        se_note("jpeg write failed: handle %d, %d bytes", o->h, n);
        o->err = 1; return -1;
    }
    o->bytes += n;
    o->cur ^= 1;
    return 0;
}

static int jpeg_write(void *ctx, const uint8_t *data, size_t len)
{
    jpeg_out *o = (jpeg_out *)ctx;
    size_t done = 0;

    while (done < len) {
        int room = SE_OUTHALF - o->len;
        int take = (int)(len - done);
        if (take > room) take = room;
        memcpy(o->buf[o->cur] + o->len, (const uint8_t *)data + done, (size_t)take);
        o->len += take;
        done   += (size_t)take;
        if (o->len == SE_OUTHALF && jpeg_flush(o) != 0) return -1;
    }
    return 0;
}

/* Feeds the JPEG decoder from the CR2, with offsets relative to the start
 * of the embedded preview rather than the file. */
typedef struct { int h; uint32_t base; } preview_rd;

/*
 * Every compressed byte the decoder is given, hashed on the way past.
 *
 * This is the instrument that was missing. Twice now a frame has come off
 * the camera wrong while the desktop renders the same raw correctly, and
 * each time the question "did we read the right bytes off the card?" has
 * been answered by reasoning rather than by measurement -- wrongly, both
 * times. The hash covers exactly the bytes handed to the decoder, so the
 * desktop can compute the same number over the same range:
 *
 *   same hash  -> the card gave us the right data and the fault is above
 *                 the reader, in the decode or what runs beside it
 *   different  -> the reads are wrong, whatever they claim to return
 *
 * It costs one multiply per byte over about 1.5 MB, against a develop that
 * takes forty seconds.
 */
static uint32_t se_rd_hash;
static uint32_t se_rd_bytes;
static int      se_rd_calls, se_rd_fails;

static int preview_read(void *ctx, uint32_t off, void *dst, uint32_t n)
{
    preview_rd *r = (preview_rd *)ctx;
    int got = read_at(r->h, (int)(r->base + off), dst, (int)n);

    se_rd_calls++;
    if (got != (int)n) { se_rd_fails++; return -1; }
    se_rd_hash   = se_hash_bytes(se_rd_hash, dst, (int)n);
    se_rd_bytes += n;
    return 0;
}

/*
 * Progress, from inside the develop.
 *
 * The reports used to be written only from the watcher's idle loop, which
 * is unreachable while a develop is running -- and the watcher takes the
 * newest frame on every power-on, so a 47-second develop started before the
 * camera had finished booting and the log never got past "developing". A
 * whole trip produced no probe lines for that reason alone.
 */
static void on_progress(void *ctx, const char *stage, int pct)
{
    se_stage = stage;
    se_pct   = pct;

    /*
     * Nothing else. This used to write the log every ten percent, which
     * meant creating, writing and closing a file on the card in the middle
     * of streaming a frame off it -- giving the card something else to do
     * at exactly the moment the decoder needed it, on a firmware whose FIO
     * has already produced three undocumented behaviours.
     *
     * The interface does not need it either: the display callback reads
     * these two variables straight out of memory, so progress is on screen
     * without anything touching the card. The reports are written when the
     * develop is over.
     */
}

/*
 * The CR2 header. The IFD0 entries that locate the preview, and the
 * MakerNote that carries the exposure data, both sit near the front of the
 * file, so a 256 KB window is ample and the rest is never touched.
 */
#define SE_HDR_BYTES (256 * 1024)

/*
 * Working memory for the decode and encode. The decoder keeps one MCU row
 * per component plus an RGB row -- about 180 KB at 2496 wide -- and the
 * encoder keeps itself and an eight-row strip.
 */
#define SE_SCRATCH (288 * 1024)


/*
 * Feeds decoded rows through the curves and into the encoder.
 *
 * There is no downscaling here any more. This used to accumulate a 2x2 box
 * filter across two decoded rows, which meant the decoder reconstructed
 * every high-frequency coefficient of a 2496x1664 frame and then this threw
 * three quarters of the result away. The decoder is asked for half size
 * instead -- see se_jpeg_decode_stream_scaled -- so what arrives is already
 * the output size and all that is left to do is the look.
 *
 * The accumulator is gone with it: 15 KB of arena and a divide per channel
 * per pixel on a core with no divide instruction.
 */
typedef struct {
    se_jpeg       *enc;
    se_look_run   *run;          /* the look itself lives in engine/look.c */
    int            ow;           /* output width, as the decoder delivers  */
    uint32_t       hash;
    int            err;
} look_ctx;

/*
 * One finished row, on its way out of the look stage.
 *
 * This is where the hash is taken, and it has to be: with halation on, rows
 * leave the look stage `radius` rows after they went in, so hashing at the
 * input would hash the wrong pixels in the wrong order and the desktop
 * check would compare two different things and still agree with itself.
 */
static int look_emit(void *ctx, const uint8_t *row, int width)
{
    look_ctx *L = (look_ctx *)ctx;

    L->hash = se_hash_bytes(L->hash, row, width * 3);
    if (se_jpeg_write_rows(L->enc, row, 1) != SE_OK) {
        if (!L->err) se_note("encoder rejected a row");
        L->err = 1; return -1;
    }
    return 0;
}

static int look_rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    look_ctx *L = (look_ctx *)ctx;

    /*
     * The encoder and the look run were both opened for L->ow columns, so a
     * decoder that delivered some other width would walk off the end of the
     * look's row buffers. It cannot happen -- both sizes come from
     * se_jpeg_scaled_size -- which is exactly why it is worth one
     * comparison a row to be sure.
     */
    if (width != L->ow) {
        if (!L->err) se_note("decoder gave %d columns, expected %d", width, L->ow);
        L->err = 1; return -1;
    }

    if (se_look_rows(L->run, rgb, nrows, look_emit, L) != SE_OK) {
        if (!L->err) { se_note("look stage rejected a row"); L->err = 1; }
        return -1;
    }
    return 0;
}

static se_err develop_preview(const char *cr2_path, const char *out_path,
                              void *hdr)
{
    se_arena     arena, lookar;
    se_look      lk;
    size_t       lookneed;
    uint32_t     t0, t_hdr, t_setup, t_stream;   /* ticks, 10 ms each */
    uint8_t     *block = 0, *scratch = 0;
    uint8_t     *pv = 0;
    se_cr2_info  info;
    se_jdec_info ji;
    se_jpeg_exif ex;
    se_jpeg     *enc = 0;
    look_ctx     L;
    jpeg_out     out;
    se_err       rc;
    preview_rd   rd;
    uint32_t     poff = 0, plen = 0;
    int          pw = 0, ph = 0, ow, oh, got, dscale;

    /* Both of these are locals with several fields; clearing them whole is
     * what keeps a stray `len` or `err` from deciding the outcome. */
    memset(&L, 0, sizeof L);
    memset(&out, 0, sizeof out);
    out.h = -1;
    rd.h  = -1;

    /* --- locate the preview and read the exposure data ----------------- */
    t0 = (uint32_t)CAM_CALL0(CAM_tickGet, long);
    got = read_file(cr2_path, hdr, SE_HDR_BYTES);
    if (got <= 0) return SE_E_IO;

    t_hdr = (uint32_t)CAM_CALL0(CAM_tickGet, long);
    rc = se_cr2_find_preview(hdr, (size_t)got, &poff, &plen, &pw, &ph);
    if (rc != SE_OK) { se_note("no preview in %s (rc=%d)", cr2_path, rc); return rc; }
    if (se_cr2_parse(hdr, (size_t)got, &info) != SE_OK)
        memset(&info, 0, sizeof info);

    /*
     * The preview's own header is already in hand: it starts at `poff`,
     * which on every frame seen so far is well inside the 256 KB of CR2
     * read to find it. The decoder needs only the markers up to the scan.
     *
     * This is read before anything is allocated because the output size
     * decides what the look stage costs, and that cost is part of the one
     * allocation below. `block` is still 0, so the error path is safe.
     */
    if (poff >= (uint32_t)got) { rc = SE_E_UNSUPPORTED; goto done; }
    pv = (uint8_t *)hdr + poff;
    if (se_jpeg_dec_info(pv, (size_t)got - poff, &ji) != SE_OK) {
        rc = SE_E_BADFILE; goto done;
    }

    /*
     * 0 is the default and the small one: the point of this is a quick
     * shareable frame, not a second full-size copy of the picture.
     *
     * The scale is handed to the decoder rather than applied afterwards. At
     * half size it runs a 4-point inverse DCT over each block's top-left
     * 4x4 coefficients instead of the full 8-point transform over all 64 --
     * an eighth of the transform work, a quarter of the colour conversion,
     * a quarter of the pixels through the look and a quarter through the
     * encoder. Nothing is reconstructed only to be averaged away.
     */
    dscale = se_res_index ? 1 : 2;
    se_jpeg_scaled_size(ji.width, ji.height, dscale, &ow, &oh);

    /*
     * The look, and what it costs. Grain and halation are in output pixels,
     * so both are scaled by the decode scale -- otherwise a half-size frame
     * would come out twice as grainy with half the glow.
     */
    se_look_from_stock(&se_stocks[se_stock_index], dscale, &lk);
    {
        int r0 = lk.hal_radius, a0 = lk.hal_amount;
        lookneed = se_look_fit(&lk, ow, SE_LOOK_BUDGET);
        if (a0 > 0 && lk.hal_amount == 0)
            se_note("halation off at %d wide: it does not fit %d KB",
                    ow, (int)(SE_LOOK_BUDGET >> 10));
        else if (lk.hal_radius != r0)
            se_note("halation radius %d -> %d to fit %d KB at %d wide",
                    r0, lk.hal_radius, (int)(SE_LOOK_BUDGET >> 10), ow);
    }

    /* --- working memory ------------------------------------------------ */
    /*
     * Small, and deliberately so. Holding the preview meant asking for about
     * 2.2 MB in one piece, and the camera refused that with 2264 KB reported
     * free: the total exists but no single run that large does. Nothing here
     * is larger than a few hundred KB, and the preview is pulled through a
     * window instead of being held.
     */
    {
        size_t need = SE_SCRATCH + SE_OUTBUF + lookneed + 64;
        block = (uint8_t *)cam_alloc(need);
        if (!block) {
            int total = 0, freeb = 0;
            cam_meminfo(&total, &freeb);
            se_note("develop: need %u bytes, %d KB free",
                    (unsigned)need, freeb >> 10);
            return SE_E_NOMEM;
        }
        out.buf[0] = block;
        out.buf[1] = block + SE_OUTHALF;
        out.cur    = 0;
        scratch    = block + SE_OUTBUF;
        se_arena_init(&arena,  scratch, SE_SCRATCH);
        se_arena_init(&lookar, scratch + SE_SCRATCH, lookneed);
    }

    se_rd_hash  = FNV_INIT;
    se_rd_bytes = 0;
    se_rd_calls = se_rd_fails = 0;

    rd.h    = cam_open(cr2_path);
    rd.base = poff;
    if (rd.h == -1) { se_note("cannot reopen the raw"); rc = SE_E_IO; goto done; }

    /*
     * The decoder keeps one MCU row per component plus an RGB row; the
     * encoder keeps itself and an eight-row strip. Roughly 400 KB at this
     * size, against the 7 MB Magic Lantern leaves of Canon's heap.
     */
    enc = SE_NEW(&arena, se_jpeg, 1);
    if (!enc) { rc = SE_E_NOMEM; goto done; }

    /* The look runs out of its own arena, so the delay line and the
     * decoder's MCU rows can never starve each other. */
    rc = se_look_begin(&L.run, &lk, ow, &lookar);
    if (rc != SE_OK) {
        se_note("look stage refused %u bytes at %d wide",
                (unsigned)lookneed, ow);
        goto done;
    }
    L.ow   = ow;
    L.hash = FNV_INIT;

    /* --- encode -------------------------------------------------------- */
    out.h = cam_create(out_path);
    out.bytes = 0;
    if (out.h == -1) { se_note("cannot create %s", out_path); rc = SE_E_IO; goto done; }

    memset(&ex, 0, sizeof ex);
    ex.make          = info.make[0] ? info.make : "Canon";
    ex.model         = info.model[0] ? info.model : "Canon EOS 5D";
    ex.software      = se_stocks[se_stock_index].name;
    ex.datetime      = info.datetime;
    ex.exposure_time = info.exposure_time;
    ex.aperture      = info.aperture;
    ex.iso           = info.iso;

    {
        uint8_t *strip = SE_NEW(&arena, uint8_t, (size_t)8 * ow * 3);
        if (!strip) { rc = SE_E_NOMEM; goto done; }
        rc = se_jpeg_begin(enc, ow, oh, 88, strip, &ex, jpeg_write, &out);
        if (rc != SE_OK) goto done;
    }
    L.enc = enc;

    t_setup = (uint32_t)CAM_CALL0(CAM_tickGet, long);
    rc = se_jpeg_decode_stream_scaled(pv, (size_t)got - poff, plen,
                                      preview_read, &rd, dscale, &arena,
                                      look_rows, &L, &ji);
    t_stream = (uint32_t)CAM_CALL0(CAM_tickGet, long);
    if (rc != SE_OK) se_note("preview decode rc=%d", rc);
    /*
     * The delay line is still holding `radius` rows. Without this the JPEG
     * is short by that many rows at the bottom and the encoder, which was
     * opened for `oh`, never gets the height it was promised.
     */
    if (rc == SE_OK && !L.err) rc = se_look_end(L.run, look_emit, &L);
    if (rc == SE_OK && !L.err) rc = se_jpeg_end(enc);
    if (rc == SE_OK && jpeg_flush(&out) != 0) rc = SE_E_IO;
    if (out.err) rc = SE_E_IO;
    /*
     * Not static, and recorded rather than only logged.
     *
     * tools/simtest took this number by grepping ML/SE.LOG for "look hash",
     * and se_note is explicitly allowed to drop a line it cannot take the
     * log lock for -- so a dropped line read as a wrong hash and the suite
     * failed about one run in eight on a develop that was byte-perfect.
     * The raw path's checksums were moved off the log for this exact
     * reason; this one was left behind. camsim reads it from here now.
     */
    if (rc == SE_OK) {
        se_last_look_hash = L.hash;
        se_note("look hash %08X, %d KB", L.hash, out.bytes >> 10);

    /*
     * Where the time actually went.
     *
     * The develop is about 2,900 cycles per output pixel, which is far
     * more than the look's arithmetic can account for -- so the cost is
     * the JPEG codec or the card, and guessing which would be guessing.
     * Four numbers settle it. Ticks are hundredths of a second.
     *
     *   hdr     reading 256 KB of CR2 to find the preview: pure card
     *   setup   allocating, baking the look, opening the encoder
     *   stream  decode + look + encode, interleaved and inseparable
     *   tail    finishing the encoder and flushing to the card
     */
    {
        uint32_t t_end = (uint32_t)CAM_CALL0(CAM_tickGet, long);
        se_note("time: hdr %u setup %u stream %u tail %u (cs, total %u)",
                (unsigned)(t_hdr - t0), (unsigned)(t_setup - t_hdr),
                (unsigned)(t_stream - t_setup), (unsigned)(t_end - t_stream),
                (unsigned)(t_end - t0));
    }
    }

done:
    if (out.h != -1) cam_close(out.h);
    if (rc != SE_OK && out.h != -1) cam_remove(out_path);
    if (rd.h != -1) cam_close(rd.h);
    if (block) cam_free(block);
    return rc;
}

/* ===================================================================== */
/* The raw spool                                                          */
/* ===================================================================== */
/*
 * Only the raw source needs this. A decoded 5D Classic frame is
 * 4476 x 2954 uint16 = 25 MB, which this body cannot be relied on to hand
 * out: Magic Lantern shrinks Canon's heap to 7 MB here and the coded file
 * alone is about 11. So both ends stream -- the coded file forward through
 * a small window, the decoded samples straight out to the card.
 *
 * The CR2's lossless-JPEG payload is sliced into two vertical halves, and
 * the decoder emits exactly two runs per coded row, each a complete
 * 2238-sample half row, in increasing row order within a half. That is the
 * whole reason this works: each half appends to its own file with no
 * seeking, and the develop pass streams bands back out of the pair.
 *
 * Cost is 25 MB written and 25 MB read per frame, which is why this is not
 * the default and why a frame takes minutes. It is also the only shape
 * that cannot run out of memory, and it reuses the develop pipeline
 * exactly as the host suite validates it byte for byte.
 */
#define SE_SPOOL_0 "A:/ML/FE0.TMP"
#define SE_SPOOL_1 "A:/ML/FE1.TMP"

/*
 * 48 KB per image half, one allocation, used two different ways.
 *
 * Writing: as TWO 24 KB buffers, filled alternately. Canon's
 * FIO_WriteFile returns before the data has left the caller's buffer, so
 * whatever is written into that buffer next lands in the tail of the file
 * the card is still reading -- the same fault that made the first camera
 * JPEGs come out banded, and the reason the finished file goes out through
 * a double buffer too.
 *
 * It was here as well, and it is almost certainly why the raw path has
 * never produced a correct frame on this body. The simulator models the
 * lazy write and caught it on the first run: every seventh row of the
 * spool was wrong -- the last row of each 32 KB write, its final 28 bytes
 * replaced by the start of the next row. 421 rows of 2954 in each half,
 * which is a frame that decodes, develops and encodes without a single
 * error and is quietly wrong.
 *
 * Reading: the whole 48 KB as one cache. The two phases never overlap, so
 * nothing is paid twice.
 *
 * The spool is written one half-row at a time -- 4476 bytes -- and read
 * back the same way, which measured about 270 KB/s on the body: 5908
 * writes and as many seek-and-reads, the access pattern CF cards are worst
 * at. Five rows to a write brings that down to about 600 each way.
 */
#define SE_SPOOL_BUF  49152
#define SE_SPOOL_HALF (SE_SPOOL_BUF / 2)

typedef struct {
    int      h[2];
    int      half;          /* samples per half row                      */
    int      rows;
    int      err;
    uint8_t *buf[2];        /* one block per image half                  */
    int      wcur[2];       /* which half of that block is being filled  */
    int      wlen[2];       /* bytes pending in it                       */
    int      rbase[2];      /* file offset the read buffer starts at, -1 */
    int      rlen[2];
} se_spool;

/*
 * Checksums that bracket the develop, so a bad frame says *where* it went
 * wrong rather than only that it did: the decode hash covers every sample
 * handed to the spool, the readback hash four rows pulled back out of it.
 * Both are computable on the desktop from the same raw, which splits the
 * problem three ways -- a wrong decode hash means the streaming decode is
 * misbehaving on the body, a right decode with a wrong readback means the
 * spool is, and both right means the fault is downstream.
 *
 * This is kept rather than tidied away because the raw path has never
 * finished a correct frame on this camera, and these two numbers are what
 * the next attempt will be read with.
 */
static uint32_t se_decode_hash[2];

/*
 * The raw path's two bracketing checksums, kept as state as well as logged.
 *
 * tools/simtest.sh compared these against tools/hashcheck by grepping them
 * out of ML/SE.LOG, and se_note is allowed to drop any line it cannot take
 * the log lock for -- so the suite's most valuable assertion was also one
 * of its flakiest, and it got flakier as the screen started logging more.
 * Same reasoning, and the same fix, as se_dump_refused and se_dev_from_raw.
 *
 * Zero means "this run never got there", which is distinguishable from any
 * real FNV value because the hash starts at FNV_INIT.
 */
volatile uint32_t se_raw_decode_hash[2];
volatile uint32_t se_raw_readback_hash;

static int spool_flush(se_spool *sp, int w)
{
    uint8_t *p = sp->buf[w] + sp->wcur[w] * SE_SPOOL_HALF;
    int      n = sp->wlen[w];
    if (!n) return 0;
    sp->wlen[w] = 0;
    /* The next row goes to the other half, so the card keeps reading this
     * one undisturbed for a whole buffer's worth of decoding. */
    sp->wcur[w] ^= 1;
    return cam_write(sp->h[w], p, (size_t)n) == n ? 0 : -1;
}

static int spool_put(se_spool *sp, int w, const void *data, int n)
{
    if (n > SE_SPOOL_HALF) return -1;        /* a row wider than a buffer */
    if (sp->wlen[w] + n > SE_SPOOL_HALF && spool_flush(sp, w) != 0) return -1;
    memcpy(sp->buf[w] + sp->wcur[w] * SE_SPOOL_HALF + sp->wlen[w],
           data, (size_t)n);
    sp->wlen[w] += n;
    return 0;
}

/* Serve `n` bytes at `off` from the read buffer, refilling it if needed. */
static int spool_get(se_spool *sp, int w, int off, void *dst, int n)
{
    if (sp->rbase[w] < 0 || off < sp->rbase[w] ||
        off + n > sp->rbase[w] + sp->rlen[w]) {
        sp->rlen[w]  = read_at(sp->h[w], off, sp->buf[w], SE_SPOOL_BUF);
        sp->rbase[w] = off;
        if (sp->rlen[w] < n) return -1;
    }
    memcpy(dst, sp->buf[w] + (off - sp->rbase[w]), (size_t)n);
    return 0;
}

static int spool_sink(void *ctx, int row, int col0, int n, const uint16_t *s)
{
    se_spool *sp = (se_spool *)ctx;
    int which = (col0 >= sp->half) ? 1 : 0;

    (void)row;
    /*
     * The sink contract guarantees whole half-rows in order, so a plain
     * append is correct. Anything else means the layout assumption above
     * is wrong, and writing it to the wrong offset anyway would produce a
     * subtly sheared image -- so refuse instead.
     */
    if (n != sp->half || (col0 != 0 && col0 != sp->half)) { sp->err = 1; return -1; }
    se_decode_hash[which] = se_hash_bytes(se_decode_hash[which], s, n * 2);
    if (spool_put(sp, which, s, n * 2) != 0) { sp->err = 1; return -1; }
    return 0;
}

static int spool_read_rows(void *ctx, int y, int n, uint16_t *dst)
{
    se_spool *sp = (se_spool *)ctx;
    int i, k;
    for (i = 0; i < n; i++) {
        int row = y + i;
        if (row >= sp->rows) row = sp->rows - 1;   /* clamp at the bottom */
        for (k = 0; k < 2; k++) {
            int off = row * sp->half * 2;
            uint16_t *d = dst + (size_t)i * (size_t)sp->half * 2
                              + (size_t)k * sp->half;
            if (spool_get(sp, k, off, d, sp->half * 2) != 0) return -1;
        }
    }
    return 0;
}

/*
 * Develop from the sensor data.
 *
 * Nothing of Canon's rendering is used: the CFA comes out of the lossless
 * JPEG, and black level, white balance, demosaic, the camera-to-sRGB
 * matrix, the stock's curves, grain and the encode are all ours.
 *
 * The curves are baked LINEAR here, unlike the preview path. This pipeline
 * applies the tone response to scene-linear sensor data before the sRGB
 * encode, which is the domain se_stock_bake() produces. Handing it the
 * sRGB-domain bake would double-apply the transfer function -- the mirror
 * of the bug that made every curved stock dark and muddy on the preview
 * path, and worth naming here so the two never get swapped.
 */
static se_err develop_raw(const char *cr2_path, const char *out_path,
                          void *hdr)
{
    se_arena    arena;
    void       *scratch = 0;
    size_t      need;
    se_cr2_info info;
    se_dev_opts opts;
    se_raw_src  src;
    se_spool    sp;
    preview_rd  rd;
    jpeg_out    out;
    se_lut1    *tone = 0;
    se_err      rc;
    int         got, i;

    memset(&out, 0, sizeof out);
    out.h = -1;
    rd.h  = -1;
    sp.buf[0] = sp.buf[1] = 0;
    sp.wcur[0] = sp.wcur[1] = 0;
    sp.wlen[0] = sp.wlen[1] = 0;
    sp.rbase[0] = sp.rbase[1] = -1;
    sp.rlen[0] = sp.rlen[1] = 0;
    sp.h[0] = sp.h[1] = -1;
    sp.err = 0;

    /* --- geometry ------------------------------------------------------ */
    got = read_file(cr2_path, hdr, SE_HDR_BYTES);
    if (got <= 0) return SE_E_IO;
    rc = se_cr2_parse(hdr, (size_t)got, &info);
    if (rc != SE_OK) { se_note("raw: cannot parse %s (rc=%d)", cr2_path, rc); return rc; }

    if (info.slice_count < 1 || (info.raw_width & 1) || info.slice_width <= 0)
        return SE_E_UNSUPPORTED;

    sp.half = info.raw_width / 2;
    sp.rows = info.raw_height;
    if (sp.half != info.slice_width) {
        se_note("raw: %d slices of %d, half is %d -- not the layout we spool",
                info.slice_count, info.slice_width, sp.half);
        return SE_E_UNSUPPORTED;
    }
    /*
     * Size the arena from the frame just parsed, not from a guess. An
     * earlier version sized it from a synthetic se_cr2_info with only
     * raw_width and raw_height set, but se_develop_scratch_size() also
     * reads active_width -- which was therefore zero. It under-reported by
     * more than half, so every develop ran the whole decode and then failed
     * with SE_E_NOMEM at the last step: "IMG_0416.CR2 rc=-4 80s".
     */
    need    = se_develop_scratch_size(&info) + SE_OUTBUF + 64;
    scratch = cam_alloc(need);
    if (!scratch) {
        int total = 0, freeb = 0;
        cam_meminfo(&total, &freeb);
        se_note("raw: need %u bytes, %d KB free", (unsigned)need, freeb >> 10);
        return SE_E_NOMEM;
    }
    out.buf[0] = (uint8_t *)scratch;
    out.buf[1] = (uint8_t *)scratch + SE_OUTHALF;
    out.cur    = 0;
    se_arena_init(&arena, (uint8_t *)scratch + SE_OUTBUF, need - SE_OUTBUF - 64);

    sp.buf[0] = (uint8_t *)cam_alloc(SE_SPOOL_BUF);
    sp.buf[1] = (uint8_t *)cam_alloc(SE_SPOOL_BUF);
    if (!sp.buf[0] || !sp.buf[1]) { rc = SE_E_NOMEM; goto done; }

    /* --- decode to the spool ------------------------------------------- */
    rd.h    = cam_open(cr2_path);
    rd.base = 0;
    if (rd.h == -1) { rc = SE_E_IO; goto done; }

    cam_remove(SE_SPOOL_0);
    cam_remove(SE_SPOOL_1);
    sp.h[0] = cam_create(SE_SPOOL_0);
    sp.h[1] = cam_create(SE_SPOOL_1);
    if (sp.h[0] == -1 || sp.h[1] == -1) { rc = SE_E_IO; goto done; }

    se_rd_hash  = FNV_INIT;
    se_rd_bytes = 0;
    se_rd_calls = se_rd_fails = 0;
    se_decode_hash[0] = se_decode_hash[1] = FNV_INIT;

    se_arena_reset(&arena);
    rc = se_cr2_decode_stream(preview_read, &rd, &info, spool_sink, &sp, &arena);
    cam_close(rd.h);
    rd.h = -1;
    /* the tail of each half is still sitting in its write buffer */
    if (rc == SE_OK && (spool_flush(&sp, 0) != 0 || spool_flush(&sp, 1) != 0))
        rc = SE_E_IO;
    if (rc != SE_OK || sp.err) {
        se_note("raw: decode rc=%d err=%d", rc, sp.err);
        if (rc == SE_OK) rc = SE_E_IO;
        goto done;
    }

    for (i = 0; i < 2; i++) { cam_close(sp.h[i]); sp.h[i] = -1; }
    sp.h[0] = cam_open(SE_SPOOL_0);
    sp.h[1] = cam_open(SE_SPOOL_1);
    if (sp.h[0] == -1 || sp.h[1] == -1) { rc = SE_E_IO; goto done; }

    se_raw_decode_hash[0] = se_decode_hash[0];
    se_raw_decode_hash[1] = se_decode_hash[1];
    se_note("decode hash L=%08X R=%08X", se_decode_hash[0], se_decode_hash[1]);

    /* Four rows back out of the spool. If the decode hash is right and this
     * one is not, the readback is where it breaks. */
    {
        /*
         * The probe row comes out of the arena rather than bss. A static
         * buffer wide enough for a half row is 4.5 KB, and the payload
         * region ends where the menu stack begins -- it overflowed the
         * linker script by almost exactly that. The arena is reset on the
         * next line anyway, so this costs nothing at all.
         */
        static const int pr[4] = { 0, 1000, 2000, 2900 };
        uint32_t rh = FNV_INIT;
        int q, k, bad = 0;
        uint16_t *probe;

        se_arena_reset(&arena);
        probe = SE_NEW(&arena, uint16_t, (size_t)sp.half);
        if (!probe) { rc = SE_E_NOMEM; goto done; }

        for (q = 0; q < 4 && !bad; q++)
            for (k = 0; k < 2; k++) {
                if (pr[q] >= sp.rows) continue;
                if (spool_get(&sp, k, pr[q] * sp.half * 2,
                              probe, sp.half * 2) != 0) { bad = 1; break; }
                rh = se_hash_bytes(rh, probe, sp.half * 2);
            }
        if (!bad) se_raw_readback_hash = rh;
        se_note("readback hash %08X%s", rh, bad ? " (A READ FAILED)" : "");
        /* the probe moved the cursor; make the cache refill from scratch */
        sp.rbase[0] = sp.rbase[1] = -1;
    }

    src.mem       = 0;
    src.read_rows = spool_read_rows;
    src.ctx       = &sp;

    /*
     * The curves are 24,576 bytes and they are neither a local nor static.
     *
     * Not a local: that is exactly the size this task's whole stack once
     * was, and the array alone consumed all of it on entry -- everything
     * deeper wrote through into bss, where se_stocks lives, so
     * se_stock_bake read curves it had itself corrupted.
     *
     * Not static either, any more: only this path needs them now, the
     * preview path bakes straight to 768 bytes of byte tables, and the
     * payload region had run out of room for a 24 KB buffer that one
     * source in two uses at all.
     */
    tone = (se_lut1 *)cam_alloc(3 * sizeof(se_lut1));
    if (!tone) { rc = SE_E_NOMEM; goto done; }
    se_stock_bake(&se_stocks[se_stock_index], tone);

    se_dev_opts_default(&opts);
    opts.scale        = se_res_index ? 1 : 2;
    opts.tone         = tone;
    opts.stock_name   = se_stocks[se_stock_index].name;
    opts.jpeg_quality = 88;
    opts.dither       = 1;

    /*
     * Grain, which this path has never had.
     *
     * se_dev_opts_default memsets, so grain_amount was 0 and nothing here
     * ever set it -- the same defect the preview path carried, where the
     * stock's GRAIN line was read, stored, and then not used. A stock
     * developed from the sensor data came out clean no matter what it
     * asked for.
     *
     * se_stock_grain honours a [RAW] override, which matters here more
     * than anywhere: this path hands mosaiced samples to DIGIC and grain
     * applied before demosaic does not look like grain applied after it,
     * so a stock may well want a different number.
     */
    opts.grain_amount    = se_stock_grain(&se_stocks[se_stock_index],
                                          SE_SRC_RAW);
    opts.grain_size      = 22;
    opts.grain_frequency = 60;

    /* --- develop ------------------------------------------------------- */
    out.h = cam_create(out_path);
    out.bytes = 0;
    if (out.h == -1) { se_note("cannot create %s", out_path); rc = SE_E_IO; goto done; }

    se_arena_reset(&arena);
    rc = se_develop(&info, &src, 0, &opts, &arena, jpeg_write, &out);
    if (rc == SE_OK && jpeg_flush(&out) != 0) rc = SE_E_IO;
    if (out.err) rc = SE_E_IO;
    if (rc == SE_OK) se_note("raw develop done, %d KB", out.bytes >> 10);

done:
    if (out.h != -1) cam_close(out.h);
    if (rc != SE_OK && out.h != -1) cam_remove(out_path);
    if (rd.h != -1) cam_close(rd.h);
    for (i = 0; i < 2; i++) if (sp.h[i] != -1) cam_close(sp.h[i]);
    if (tone) cam_free(tone);
    if (sp.buf[0]) cam_free(sp.buf[0]);
    if (sp.buf[1]) cam_free(sp.buf[1]);
    if (scratch) cam_free(scratch);
    cam_remove(SE_SPOOL_0);
    cam_remove(SE_SPOOL_1);
    return rc;
}

/*
 * Which source develops this frame.
 *
 * The switch is read here, once per frame, rather than captured when the
 * watcher starts: turning the wheel between two shots should change the
 * next one, the same way the stock does.
 */
/*
 * Which path each frame actually went down, counted.
 *
 * Not static: tools/camsim asserts on these instead of grepping the log
 * for "developing ... from raw". Two reasons that grep was the wrong
 * assertion. It read a line se_note is allowed to drop when it cannot take
 * the log lock, so it was flaky -- and it asserted *intent* at the moment
 * the line was written, where what the test wants to know is which code
 * ran. se_src_index itself cannot answer that after the fact either: the
 * wheel and the joystick change it between frames, and camsim's own button
 * tests flip it before the run ends.
 */
volatile int se_dev_from_raw, se_dev_from_preview;

static se_err develop_one(const char *cr2_path, const char *out_path,
                          void *hdr)
{
    if (se_src_index == SE_SOURCE_RAW) {
        se_dev_from_raw++;
        return develop_raw(cr2_path, out_path, hdr);
    }
    se_dev_from_preview++;
    return develop_preview(cr2_path, out_path, hdr);
}

/* ===================================================================== */
/* The watcher                                                            */
/* ===================================================================== */

/*
 * A frame is "done" when a .FLM.JPG sits beside it, so the record of what
 * has been developed lives on the card rather than in memory. That survives
 * a battery change, costs nothing to maintain, and means the watcher and a
 * full sweep are the same operation looking at different sets of files.
 */
static void out_path_for(const char *cr2, char *out, size_t n)
{
    size_t l;
    str_copy(out, n, cr2);
    l = strlen(out);
    if (l > 4) str_copy(out + l - 4, n - (l - 4), ".FLM.JPG");
}

static int already_done(const char *out)
{
    uint32_t sz = 0;
    CAM_CALL2(CAM_FIO_GetFileSize, int, out, &sz);
    return sz > 0;
}

/* The port sets a 100 Hz clock (sysClockRateSet_100), so ticks/100 is
 * seconds -- good enough to report how long a frame took. */
static int now_secs(void) { return (int)CAM_CALL0(CAM_tickGet, long) / 100; }

typedef struct {
    void     *hdr;
    int       sweep;      /* 1 = develop everything, 0 = only new frames */
    int       census;     /* 1 = just record the watermark, develop nothing */
    int       probe;      /* 1 = count what needs developing, allocate nothing */
    int       found;
    char      newest[SE_PATH_MAX];   /* census only */
} job_ctx;

static void on_cr2(const char *dir, const struct fio_file *f, void *ctx)
{
    job_ctx *j = (job_ctx *)ctx;
    char in[SE_PATH_MAX], out[SE_PATH_MAX];
    se_err rc;
    int t0;

    if (f->mode & ATTR_DIRECTORY) return;
    if (!ends_with_ci(f->name, ".CR2")) return;

    se_snprintf(in, sizeof(in), "%s%s", dir, f->name);

    /*
     * Census pass: find the two newest frames and develop nothing.
     *
     * The mark is set to the *second* newest, so the newest frame is always
     * eligible. That is what lets a develop interrupted by the camera going
     * to sleep be picked up again on the next power-on -- it left no
     * .FLM.JPG behind, so it still counts as pending, and already_done()
     * skips it the moment it does have one. Marking the newest instead would
     * retire a frame that was never finished.
     */
    if (j->census) {
        if (strcmp(in, j->newest) > 0) {
            str_copy(se_watermark, sizeof(se_watermark), j->newest);
            str_copy(j->newest, sizeof(j->newest), in);
        } else if (strcmp(in, se_watermark) > 0) {
            str_copy(se_watermark, sizeof(se_watermark), in);
        }
        return;
    }

    /* Auto develop only looks at frames newer than the boot watermark. */
    if (!j->sweep && strcmp(in, se_watermark) <= 0) return;

    out_path_for(in, out, sizeof(out));
    if (already_done(out)) return;

    /*
     * Do not touch a frame the camera may still be writing. A complete 5D
     * Classic raw is around 11 MB; anything much smaller is a file in
     * flight, and developing it would both fail and contend with the write.
     */
    if (f->size < 6u * 1024 * 1024) return;

    j->found++;
    if (j->probe) return;        /* counting only; nothing allocated yet */

    se_snprintf(se_status, sizeof(se_status), "%s", f->name);
    se_pct = 0;
    /* The stock goes in the line. Working out which look a frame off the
     * camera was given, by hashing the raw against all eleven on the
     * desktop, has already cost one round of analysis -- and the wheel can
     * change it between frames. */
    se_note("developing %s (%u bytes) from %s stock=%s scale=%d", f->name,
            (unsigned)f->size,
            se_src_index == SE_SOURCE_RAW ? "raw" : "preview",
            se_stocks[se_stock_index].name,
            se_res_index ? 1 : 2);

    t0 = now_secs();
    rc = develop_one(in, out, j->hdr);
    se_last_secs = now_secs() - t0;
    se_note("stream %08X %u bytes, %d reads, %d short",
            (unsigned)se_rd_hash, (unsigned)se_rd_bytes, se_rd_calls,
            se_rd_fails);
    se_note("%s rc=%d %ds", f->name, rc, se_last_secs);
    if (!se_canary_ok()) {
        se_note("STACK OVERFLOW during develop -- output is not trustworthy");
        se_canary_set();
    }

    if (rc == SE_OK) { se_done++; se_q_done++; } else se_failed++;

    /*
     * Never claim a total smaller than the frame being worked on.
     *
     * se_sweep's probe counts the queue once, and then each develop takes
     * the best part of twenty seconds -- long enough to shoot several more
     * frames, which the walk picks up in the SAME sweep. The count was
     * taken before those existed, so the screen read "FRAME 2 OF 1" and
     * then "FRAME 3 OF 1".
     *
     * There is no honest way to know what is still coming: the camera may
     * be mid-burst. So the total grows to meet the work -- 1 of 1, then
     * 2 of 2 -- which says "there was more than I thought" rather than
     * something arithmetically impossible.
     */
    if (se_q_done >= se_q_total) se_q_total = se_q_done;

    /*
     * Advance the mark past this frame whether or not it worked. A frame
     * that fails leaves no .FLM.JPG behind, so without this the watcher
     * would retry the same broken file every two seconds forever.
     */
    if (strcmp(in, se_watermark) > 0)
        str_copy(se_watermark, sizeof(se_watermark), in);
}

static void on_dcim_dir(const char *dir, const struct fio_file *f, void *ctx)
{
    char sub[SE_PATH_MAX];
    if (!(f->mode & ATTR_DIRECTORY)) return;
    se_snprintf(sub, sizeof(sub), "%s%s/", dir, f->name);
    se_walk(sub, on_cr2, ctx);
}

/*
 * One pass over the card. Returns how many frames it developed.
 *
 * Working memory is held only for the duration of a pass, not for the life
 * of the camera: the arena is a few hundred KB, but the develop itself asks
 * for up to 25 MB, and holding any of that idle between shots would be the
 * difference between Canon having room to buffer a burst and not.
 */
static int se_sweep(int sweep)
{
    job_ctx  j;
    void    *hdr;
    int      total = 0, freeb = 0;

    /*
     * Look before allocating. The watcher wakes every couple of seconds and
     * almost always has nothing to do, and the develop needs the best part
     * of a megabyte -- taking and returning that from a 7 MB heap on every
     * idle pass would be a lot of churn for nothing, and would compete with
     * Canon for memory it needs to buffer a burst.
     */
    j.hdr    = 0;
    j.sweep  = sweep;
    j.census = 0;
    j.probe  = 1;
    j.found  = 0;
    se_walk("A:/DCIM/", on_dcim_dir, &j);
    if (j.found == 0) return 0;

    /* what the probe just learned, before it is overwritten below */
    se_q_total = j.found;
    se_q_done  = 0;

    hdr = cam_alloc(SE_HDR_BYTES);
    if (!hdr) {
        cam_meminfo(&total, &freeb);
        se_note("sweep: no memory for the header window, %d KB free",
                freeb >> 10);
        se_snprintf(se_status, sizeof(se_status), "no memory (%d KB free)",
                    freeb >> 10);
        return 0;
    }

    se_set_progress(on_progress, 0);

    j.hdr   = hdr;
    j.probe = 0;
    j.found = 0;
    se_walk("A:/DCIM/", on_dcim_dir, &j);

    se_set_progress(0, 0);
    cam_free(hdr);
    return j.found;
}

/* Record the newest frame already on the card. Allocates nothing. */
static void se_census(void)
{
    job_ctx j;
    j.hdr    = 0;
    j.sweep  = 0;
    j.census = 1;
    j.probe  = 0;
    j.found  = 0;
    j.newest[0] = '\0';
    se_watermark[0] = '\0';
    se_walk("A:/DCIM/", on_dcim_dir, &j);
}

/*
 * The watcher task. Started from se_boot_init, so it is running before the
 * first shot and needs no visit to the menu to arm it.
 */
void se_watch(void)
{
    /*
     * Let Canon finish booting before touching the card. This runs from ML's
     * init task chain, which is early -- the card may not be mounted yet,
     * and a census that finds nothing would treat every existing frame as
     * new.
     */
    se_note("watcher task running");
    se_canary_set();

    /*
     * The screen first, and the card afterwards.
     *
     * The five-second sleep below is here because Canon may not have
     * mounted the card yet, and a census that found nothing would treat
     * every existing frame as new. That is a reason to wait before
     * touching the CARD. It is not a reason to wait before being able to
     * draw -- and because the frame buffer used to be allocated after it,
     * AND after se_scan_stocks reads eleven files, pressing DELETE in the
     * first six seconds after power-on did nothing at all. se_screen's
     * first guard is `if (se_scr_want && !se_scr_open && !se_fb) continue`.
     *
     * That was the "takes a few seconds to show up". It was never the open
     * path: with the card write off that, the camera measured
     * "open took 180 ms (canon 70, first frame 110)".
     *
     * AllocateMemory is a heap call, not card I/O, and the heap is up
     * before our payload is even loaded -- ML's own init mallocs its idle
     * buffer at 0x00980464 before chaining to us. So this is safe here.
     */
    se_ui_init(&se_uistate);
    if (!se_fb) se_fb = (uint8_t *)cam_alloc(SE_UI_FB_SIZE);
    /*
     * NO LOGGING HERE. se_note is a FIO_CreateFile on the card, and the
     * sleep below exists precisely because the card may not be mounted
     * yet -- putting a card write above it undoes the thing it is for.
     * The result is reported after the wait instead.
     */

    /*
     * Wait for the card by ASKING, not by sleeping for five seconds.
     *
     * The wait is here because Canon may not have mounted the card yet,
     * and a census that found nothing would treat every existing frame as
     * new. That is a reason to wait for the FILESYSTEM -- so wait for the
     * filesystem, and stop as soon as it answers.
     *
     * It matters more than it looks. Standby on this body is not a sleep,
     * it is a restart: ML's init runs again, which re-runs our loader,
     * which re-runs this function. So every wake from standby paid the
     * full five seconds before the picker would list anything, on a card
     * that had been mounted the whole time.
     *
     * INDEX.TXT is the right thing to ask for: it is the file the scan
     * needs next, and DCIM lives on the same filesystem.
     */
    {
        uint32_t sz = 0;
        int      waited = 0;
        for (;;) {
            CAM_CALL2(CAM_FIO_GetFileSize, int, "A:/ML/FILM/INDEX.TXT", &sz);
            if (sz) break;
            if (waited >= 5000) break;      /* the old blind wait, as a cap */
            cam_msleep(100);
            waited += 100;
        }
        se_card_ms = waited;
    }

    se_note("watcher alive, card ready in %d ms, frame buffer %s (%d bytes)",
            se_card_ms, se_fb ? "ready before the wait" : "NOT YET",
            SE_UI_FB_SIZE);
    if (!se_fb) {
        /* the heap should be up long before this -- ML's own init mallocs
         * 86,400 bytes at 0x00980464 before chaining to us -- but a screen
         * that can never open is not a thing to leave to should */
        se_fb = (uint8_t *)cam_alloc(SE_UI_FB_SIZE);
        se_note("frame buffer %s on the retry",
                se_fb ? "ready" : "REFUSED -- the screen cannot open");
    }

    se_cfg_load();                 /* source and size apply immediately */
    se_scan_stocks();
    se_cfg_apply_stock();          /* the stock needs the list first */
    se_uistate.ready = 1;          /* the card has been looked at */
    se_note("stocks found: %d", se_nstocks);

    {
        uint32_t sz = 0;
        CAM_CALL2(CAM_FIO_GetFileSize, int, "A:/ML/SE.RAW", &sz);
        if (sz) {
            se_src_index = SE_SOURCE_RAW;
            se_note("ML/SE.RAW present: developing from the sensor data");
        }
    }

    /*
     * ML/SE.ALL: develop everything on the card, once.
     *
     * This used to be what SET did, which meant one thumb press on a
     * picker could start an hour of work on a full card with no warning
     * and no way back. It is a real capability -- a backlog of raws, a
     * stock you have just retuned -- so it keeps a way in, but a
     * deliberate one, in the same idiom as SE.RAW and SE.NOUI.
     *
     * The file is removed as soon as it is seen, so it runs once rather
     * than on every power-on until somebody remembers.
     */
    {
        uint32_t sz = 0;
        CAM_CALL2(CAM_FIO_GetFileSize, int, "A:/ML/SE.ALL", &sz);
        if (sz) {
            se_request_all = 1;
            cam_remove("A:/ML/SE.ALL");
            se_note("ML/SE.ALL present: developing every frame on the card, "
                    "once (the file has been removed)");
        }
    }

    /*
     * ML/SE.UI opens the interface without needing a button, so the screen
     * can be tested while the button path is still being measured.
     */
    {
        uint32_t sz = 0;
        CAM_CALL2(CAM_FIO_GetFileSize, int, "A:/ML/SE.UI", &sz);
        if (sz && !se_no_ui()) {
            se_scr_want = 1;
            se_scr_pinned = 1;
            se_note("ML/SE.UI present: opening the screen at startup");
        }
    }

    /* the frame buffer and se_ui_init are done above, before the card */
    se_note("ready, auto=%d from=%s size=%s stock=%s",
            se_auto_on,
            se_src_index == SE_SOURCE_RAW ? "raw" : "preview",
            se_res_name[se_res_index & 1],
            se_nstocks ? se_stocks[se_stock_index].name : "(none)");

    se_census();
    se_note("census mark: %s", se_watermark[0] ? se_watermark : "(none)");

    /* With the menu shut and the panel dark: exactly the state a
     * standalone engine would boot into. */

    for (;;) {
        int want = se_request_all;

        /*
         * Stop touching the card. A directory walk or a 200 KB JPEG write
         * in flight when Canon is powering down is what leaves the access
         * LED lit -- and se_note is already silent by now, so this loop
         * would otherwise spin invisibly. Same test ML's tasks use.
         */
        if (se_shutting_down()) { cam_msleep(200); continue; }

        /*
         * Save the chosen look, if a button changed it.
         *
         * Here and not in se_key: that runs on Canon's GuiMainTask, where
         * nothing may touch the filesystem. The flag is the whole of the
         * handover, and it also debounces -- spinning the wheel through
         * eleven stocks costs one write, not eleven.
         */
        if (se_cfg_dirty) { se_cfg_dirty = 0; se_cfg_save(); }

        if (se_hold_develop()) {
            static int said;
            if (!said) { said = 1; se_note("ML/SE.HOLD: developing suspended"); }
        } else if (!se_busy && (want || se_auto_on)) {
            se_busy = 1;
            se_request_all = 0;
            if (se_sweep(want) > 0) {
                se_snprintf(se_status, sizeof(se_status), "%d done, %d failed",
                            se_done, se_failed);
            }
            se_busy = 0;
        }

        /*
         * Between sweeps, write out whatever the probe has learned.
         *
         * There used to be a se_paint() here, on a 250 ms timer. It could
         * never have worked: a draw from this task goes into the buffer on
         * the panel, and ML's menu redraw copies its own idle buffer over
         * every byte of that. Drawing now happens only where it survives --
         * inside ML's redraw, from the display callback -- and this task's
         * job is to get the callback's findings onto the card, which the
         * callback itself must not do.
         *
         * A directory walk every pass would keep the card awake for nothing,
         * so the sweep itself still only runs every four seconds.
         */
        {
            int t;
            for (t = 0; t < 16; t++) {
                se_key_report();
                se_screen_report();
                se_scr_timing_report();
                se_canon_pal_report();
                /*
                 * A quarter of a second, asleep.
                 *
                 * This used to be twenty-five 10 ms ticks with a probe
                 * between them that read Canon's imaging buffers, so the
                 * watcher spent its entire idle life touching the memory
                 * DIGIC captures into. Now it does what an idle loop
                 * should: nothing, as cheaply as possible.
                 */
                cam_msleep(250);
            }
        }
    }
}

/* Called from se_boot_init, on ML's init task, before ML's own init runs. */
/*
 * Two tasks, started at boot.
 *
 * The watcher develops frames and spends eighteen seconds at a time doing
 * it; the screen has to stay responsive throughout, because the progress
 * bar is most of why there is a screen. One task could not do both.
 *
 * task_create(name, priority, stack_bytes, entry, arg). Priority 0x1F is
 * what the port uses for its own init task -- well below anything in the
 * capture path, so neither can delay a shot.
 */
void se_start(void)
{
    static const char wname[] = "film_watch";
    static const char sname[] = "film_screen";
    CAM_CALL5(CAM_task_create, void, wname, 0x1F, 0x1000, se_boot_watch, 0);
    CAM_CALL5(CAM_task_create, void, sname, 0x1F, 0x1000, se_boot_screen, 0);
}


/*
 * Every button, from Canon's GUI task via the hook spliced into
 * my_gui_task. Nothing is claimed: the stock list lives inside Magic
 * Lantern's menu now, and ML's own navigation drives it -- SET moves to the
 * next stock, which is the one control this body offers in a menu.
 *
 * The hook stays because it is proved to work, and because it is the only
 * route to input that does not depend on the menu being open. Returning 1
 * for everything leaves the camera behaving exactly as it would without it.
 */
/*
 * Every button, straight from Canon's GUI task, through the hook spliced
 * into my_gui_task. This is the one input path that has been confirmed to
 * work on the body -- the camera's log recorded events arriving here.
 *
 * Nothing is consumed: every event is passed on to Magic Lantern exactly as
 * before, so the camera behaves normally and ML's menu still works. We only
 * watch for the wheel and the arrows and move the stock selection, which is
 * additive and cannot break anything if the codes are wrong.
 */
int se_key(void *ev)
{
    const int *w;
    int param;

    if (!ev) return 1;
    w = (const int *)ev;
    param = w[1];

    se_key_trace(w);
    se_key_count(param, se_scr_want);
    se_scr_poke++;

    /*
     * Our own fake press has to reach Canon. (C1, docs/ML_DISPLAY.md 10.5)
     *
     * se_screen asks Canon for the panel the way Magic Lantern does --
     * GUI_Control(BGMT_MENU, 0, FAKE_BTN, 0) -- and that event is posted
     * into GuiMainTask's queue at MEM(0x1271C), which my_gui_task dequeues
     * and hands to THIS hook before Canon ever sees it (the bl at
     * 0x00989DB4 that patch-film.py replaces).
     *
     * The build before this one read param == 3, fell into case
     * SE_BTN_MENU, and closed the screen it had just asked to open. The
     * camera logged exactly that:
     *
     *   screen: asking Canon for the panel (fake MENU)
     *     ev 00000000 00000003 00000000 FFFE1DC0  t=3231
     *   screen: displayon=0 vram=0x105890C0
     *   screen: the panel went out, closing
     *
     * SE_FAKE_BTN is 0xFFFE1DC0, from fake_simple_button's literal pool at
     * 0x00988438 -- and the fourth word of that logged event is it.
     *
     * ML passes these through: the 2012 my_gui_task has no IS_FAKE test at
     * all, it does `ldmib r3, {r0,r1,r2}` at 0x00989E54 and hands
     * param/obj/arg straight to gui_massive_event_loop, FAKE_BTN included,
     * and Canon opens its menu anyway.
     */
    if (w[3] == SE_FAKE_BTN) return 1;

    /*
     * One button, and nothing else.
     *
     * This file is otherwise byte-identical to the last payload that ran
     * correctly on the camera -- booted, shot, developed in 15 s, every
     * button working. Three builds since then each added a little more
     * here and each came back dead, and I explained all three differently
     * and was wrong at least twice. So this changes exactly one thing: if
     * the camera survives it, consuming an event is safe and the fault is
     * in what those builds did afterwards.
     *
     * TRASH is consumed so Magic Lantern's menu stops opening, which is
     * the thing actually asked for. Nothing of ours is driven from it --
     * no screen, no display, no card. The count is reported by the
     * watcher, from a task of ours, so the log says whether the press
     * arrived without this function touching anything to say so.
     *
     * The rest of the old comment still applies:
     *
     * Two trips in a row came back with a camera whose buttons did
     * nothing, and on the second the log showed why the guess was wrong:
     * of 34 events, not one carried a code in the 0x00..0x16 range that
     * ML's gui.h calls a button, although DELETE, PLAY and MENU were all
     * pressed. The two values that did decode -- 0xC6539 and 0xDEBD9 --
     * are its HALFSHUTTER sentinels, so the offset is right and the button
     * codes genuinely were not there.
     *
     * Until a trip says what a press of DELETE looks like at this hook,
     * acting on a guess about it has cost more than it has bought. This
     * build watches and reports, and every event reaches Canon exactly as
     * it would if the hook were not installed.
     *
     * The screen is still reachable: put ML/SE.UI on the card and it opens
     * at startup. That separates "can we draw our interface" from "can we
     * read a button", which have been failing together and are not the
     * same question.
     */
    if (param == SE_BTN_TRASH) {
        se_trash_seen++;
        se_scr_want = !se_scr_want;
        return 0;                  /* so ML's menu never opens */
    }

    /*
     * Closed: the camera is a camera. Both wheels send 0x00 and 0x01 and
     * out here those are the exposure dials.
     */
    if (!se_scr_want) return 1;

    /*
     * Open: the engine's own buttons, and NOTHING ELSE. The camera showed
     * the wheel scrolling Canon's menu underneath the picker, which is
     * what not claiming it looks like.
     *
     * PLAY, INFO and JUMP still reach Canon. A build that took everything
     * left a camera with no working controls, and that is not coming back.
     */
    switch (param) {
    /* Inverted deliberately: turning the wheel the way the list reads on
     * screen should move forward through it, and on this body the code
     * named UP is the direction that felt backwards in the hand. */
    case SE_BTN_WHEEL_UP:
    case SE_BTN_WHEEL_LEFT:  se_prev_stock();    return 0;
    case SE_BTN_WHEEL_DOWN:
    case SE_BTN_WHEEL_RIGHT: se_next_stock();    return 0;

    case SE_BTN_UP:    case SE_BTN_UP_RIGHT:   case SE_BTN_UP_LEFT:
    case SE_BTN_RIGHT: case SE_BTN_LEFT:
    case SE_BTN_DOWN:  case SE_BTN_DOWN_RIGHT: case SE_BTN_DOWN_LEFT:
        se_toggle_source();
        return 0;

    /*
     * SET chooses this film, and closes.
     *
     * It used to set se_request_all, which makes the watcher sweep the
     * WHOLE card and develop every frame on it -- press SET once with 130
     * photos and the camera disappears for an hour. That is not what a
     * button called SET on a film picker means, and nothing warned.
     *
     * What it means now: this is the stock, remember it, get out of the
     * way. The remembering is a flag -- this is Canon's GUI task and may
     * not write the card (see se_cfg_save).
     *
     * Developing every frame on the card is still possible and is now a
     * deliberate act: put an empty ML/SE.ALL on the card. The same idiom
     * as ML/SE.RAW and ML/SE.NOUI, and impossible to do by accident with
     * a thumb.
     */
    case SE_BTN_SET:
        se_cfg_dirty = 1;
        se_scr_want  = 0;          /* chosen: close the picker */
        se_ui_dirty  = 1;
        return 0;

    /*
     * MENU closes the interface. The camera reported 03 arriving four
     * times with our screen open and no case for it here, so it fell
     * through to Canon -- whose menu then opened underneath ours, which
     * is what "I could not move around" looked like.
     */
    case SE_BTN_MENU:
        se_scr_want = 0;
        se_ui_dirty = 1;
        return 0;

    default:
        return 1;
    }
}
