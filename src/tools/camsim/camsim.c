/*
 * camsim.c - run the real ml/payload/film.c on the desktop.
 *
 * Implements the mock side of cam.h: a Canon-shaped file API over an
 * ordinary directory, a bounded heap, a real worker stack, and reads that
 * come up short the way the camera's do.
 *
 * usage: camsim <card-dir> [frames-to-wait-for]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include "cam.h"

uintptr_t sim_low[SIM_LOW_SIZE / 4];
uintptr_t sim_reg[SIM_REG_SIZE / 4];

/* Canon takes these around palette writes; here they only have to exist. */
/*
 * Canon's display-device lock, counted rather than ignored.
 *
 * Taking this and writing the DIGIC palette registers behind it is what a
 * previous build did more than once a second, forever, from a task with no
 * idea what the imaging path was doing -- and a camera came back with Err
 * and no working buttons. So the simulator counts it, and the run fails if
 * it happens while a capture is in flight or more than a handful of times
 * in a session. The old code would have failed on both counts.
 */
long sim_ddev_takes, sim_ddev_gives, sim_turnons;
extern const uint32_t se_palette[16];
/* fake presses the payload's own hook swallowed before Canon saw them */
long sim_fakes_eaten;
/* blits made while the palette registers were not ours */
long sim_blits_bad_palette;
/* blits made into a panel that was not on */
long sim_blits_panel_dark;
extern long sim_blits_after_teardown;
extern int se_key(void *ev);
int  sim_overlay_live;
int  sim_capture_in_flight;
long sim_ddev_during_capture;

void sim_engdrvout(long reg, long val)
{
    if (sim_capture_in_flight) sim_ddev_during_capture++;
    if (reg) MEM((uintptr_t)reg) = (uintptr_t)(uint32_t)val;
}
void sim_bmpddev_take(void)
{
    sim_ddev_takes++;
    if (sim_capture_in_flight) sim_ddev_during_capture++;
}
void sim_bmpddev_give(void) { sim_ddev_gives++; }

/*
 * Telling the bitmap display device the overlay is live. Counted with the
 * rest of the display hardware, because it is display hardware: it must
 * not happen during a capture or on a timer, only when the screen opens.
 */
long sim_notifies, sim_shows, sim_hides;

/*
 * The bitmap layer, modelled the way the ROM behaves.
 *
 * ShowBitmap does nothing unless MEM(0x0000FCA0) is 1 -- the camera's own
 * code checks it and returns -- so a payload that shows the layer before
 * turning the panel on gets nothing, here as there. And a blit only counts
 * as visible once the layer is on, which is why the picker appeared over
 * Canon's menu and nowhere else.
 */
int sim_bitmap_shown;


void sim_show_bitmap(void)
{
    sim_shows++;
    if (sim_capture_in_flight) sim_ddev_during_capture++;
    if (MEM(CAM_display_is_on) != 1) return;   /* exactly what the ROM does */
    sim_bitmap_shown = 1;
    MEM(CAM_bmp_shown) = 0xEA;
}

void sim_hide_bitmap(void)
{
    sim_hides++;
    if (sim_capture_in_flight) sim_ddev_during_capture++;
    if (MEM(CAM_display_is_on) != 1) return;
    sim_bitmap_shown = 0;
    MEM(CAM_bmp_shown) = 0x15;
}
void sim_notify_bmp_on(void)
{
    sim_notifies++;
    if (sim_capture_in_flight) sim_ddev_during_capture++;
    sim_overlay_live = 1;
}
uintptr_t sim_ml[SIM_ML_SIZE / 4];

/*
 * An address in neither region. On the camera this would be a stray write
 * into whatever happens to live there; here it is a bug we can still fix for
 * free, so it stops the run rather than returning a scratch word.
 */
uintptr_t *sim_mem_fault(uintptr_t addr)
{
    fprintf(stderr, "[sim] FATAL: the payload touched 0x%08lX, which is in "
                    "neither Canon's low RAM nor ML's region\n",
            (unsigned long)addr);
    abort();
}

/*
 * Seed ML's region with the real relocated image, so every word the payload
 * reads out of ML -- the Debug entry list, the renderer's prologue -- is the
 * word the camera has rather than one this file made up.
 *
 * That is the point of doing it at all. The last session's menu offsets were
 * four bytes out and the simulator had its own copy of the same mistake, so
 * it confirmed them. Against the real table, offsets that are wrong simply
 * do not find a display callback, and the run fails here.
 *
 * AUTOEXEC.BIN maps to 0x00980000 from file offset 0x1000; patch-film.py
 * uses the same relation.
 */
#define SIM_RESTART     0x0004B000
#define SIM_IMAGE_OFF   0x11C

static int sim_seed_ml_image(const char *path)
{
    unsigned char buf[4];
    long n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, SIM_IMAGE_OFF, SEEK_SET) != 0) { fclose(f); return 0; }
    while (n < (long)SIM_ML_SIZE && fread(buf, 1, 4, f) == 4) {
        sim_ml[n >> 2] = (uintptr_t)buf[0] | ((uintptr_t)buf[1] << 8) |
                         ((uintptr_t)buf[2] << 16) | ((uintptr_t)buf[3] << 24);
        n += 4;
    }
    fclose(f);
    return n > 0x30000;          /* the whole relocated image, not a stub */
}

/*
 * Link the Debug entries the way menu_add does. The `next` words are zero in
 * the image because the list is built at runtime, and menus_display walks it
 * through them, so without this the walk sees one entry.
 */
/* Aligned, because the camera's pointer is and both bmp_puts and the
 * payload refuse an unaligned one. An unaligned stand-in made the panel
 * check fail for a reason that does not exist on the body. */
static uint8_t sim_bmp_real[ML_BMP_BYTES] __attribute__((aligned(32)));
static uint8_t sim_bmp_idle[ML_BMP_BYTES] __attribute__((aligned(32)));
static long    st_draws, st_draw_clipped;

static long sim_bmp_nonzero(const uint8_t *b)
{
    long i, n = 0;
    for (i = 0; i < ML_BMP_BYTES; i++) if (b[i]) n++;
    return n;
}

/*
 * A stand-in for Canon's imaging state object.
 *
 * There is no DIGIC here, so this cannot tell us anything about the real
 * pipeline -- its only job is to make the payload's probe take the path it
 * will take on the camera: a plausible pointer at 0x00004F28, a struct
 * behind it, and a develop buffer field that changes when a frame is
 * "taken". A probe that silently does nothing on the desktop is a probe
 * nobody has tested.
 */
#define SIM_DEVSTATE_OBJ 0x00030000

/*
 * Stand-in buffers for the two the state object points at -- and the
 * assertion that nothing of ours reads them.
 *
 * These are mapped PROT_NONE. The state object points straight at them, so
 * the payload is entirely free to follow those pointers the way it used
 * to; the first byte it reads takes the process down with SIGSEGV and the
 * suite fails loudly, naming this as the reason.
 *
 * That is deliberate, and it is the whole point. "Never touch imaging
 * memory during capture" was written in this file's header and in the
 * payload's, and over the trips that needed a develop-seam probe the code
 * drifted away from it anyway: se_snap_poll ended up reading these buffers
 * a hundred times a second, forever, straight through every capture, and
 * the camera answered with Err on the shutter. A rule that only exists in
 * a comment is a rule that gets edited away by whoever needs it gone.
 *
 * The simulator unlocks them only to write its own capture, which is Canon
 * filling them -- never for a read.
 */
#define SIM_IMGBUF_BYTES CAM_DEVBUF_SIZE
static uint8_t *sim_imgbuf_a, *sim_imgbuf_b;

static void sim_imgbuf_protect(int writable)
{
    int prot = writable ? (PROT_READ | PROT_WRITE) : PROT_NONE;
    if (mprotect(sim_imgbuf_a, SIM_IMGBUF_BYTES, prot) != 0 ||
        mprotect(sim_imgbuf_b, SIM_IMGBUF_BYTES, prot) != 0) {
        perror("[sim] mprotect on the imaging buffers");
        exit(1);
    }
}

int sim_imgbuf_ok(uintptr_t p)
{
    uintptr_t a = (uintptr_t)sim_imgbuf_a, b = (uintptr_t)sim_imgbuf_b;
    return (p >= a && p < a + SIM_IMGBUF_BYTES) ||
           (p >= b && p < b + SIM_IMGBUF_BYTES);
}

static void sim_devstate_init(void)
{
    int i;

    sim_imgbuf_a = mmap(NULL, SIM_IMGBUF_BYTES, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    sim_imgbuf_b = mmap(NULL, SIM_IMGBUF_BYTES, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (sim_imgbuf_a == MAP_FAILED || sim_imgbuf_b == MAP_FAILED) {
        perror("[sim] mmap of the imaging buffers");
        exit(1);
    }
    for (i = 0; i < SIM_IMGBUF_BYTES; i++) {
        sim_imgbuf_a[i] = (uint8_t)(i * 7 + 1);
        sim_imgbuf_b[i] = (uint8_t)(i * 13 + 2);
    }
    sim_imgbuf_protect(0);
    MEM(CAM_DEVSTATE_PTR) = SIM_DEVSTATE_OBJ;
    for (i = 0; i < 16; i++)
        MEM(SIM_DEVSTATE_OBJ + (uintptr_t)i * 4) = 0xD0000000u + (uintptr_t)i;
    MEM(SIM_DEVSTATE_OBJ + 0x28) = (uintptr_t)sim_imgbuf_a;
    MEM(SIM_DEVSTATE_OBJ + 0x2C) = (uintptr_t)sim_imgbuf_b;
    /* +0x30 left as 0xD000000C: misaligned, so the refusal path runs too */
}

/*
 * A capture, as far as the probe can tell.
 *
 * The camera showed the buffer address does not move, so what changes is
 * the *content*: Canon develops into the same allocation each time and
 * leaves it at a uniform fill afterwards. This writes something different
 * into it so the payload's content-change detector has something to catch
 * -- without it the snapshot path never runs on the desktop.
 */
static void sim_devstate_capture(void)
{
    static uint32_t n;
    int i;
    n++;
    sim_imgbuf_protect(1);          /* Canon writing, not us reading */
    for (i = 0; i < SIM_IMGBUF_BYTES; i++)
        sim_imgbuf_a[i] = (uint8_t)(i * 3 + n * 29);
    sim_imgbuf_protect(0);
}

/*
 * The panel starts DARK, which is the state a standalone engine boots into.
 * DISPLAY_IS_ON reads zero, exactly as the camera reported.
 *
 * The BITMAP LAYER, by contrast, starts ON -- see docs/ML_DISPLAY.md 4.4.
 * This model used to start it off and wait for ShowBitmap, which is what
 * docs/FIRMWARE.md 7 said, and 7 is wrong:
 *
 *   UndisplayPhysicalScreen (0xFFB0B244) has ZERO callers in the 8 MB ROM,
 *   so HideBitmap (0xFFB09864) is never called and MEM8(0x0000FC9D) is set
 *   to 0xEA once, by the GUI's one-shot bring-up, and never to anything
 *   else. TurnOnDisplay re-applies it to the layer register at 0xFFB0933C
 *   every time the panel comes on.
 *
 * So a payload that never calls ShowBitmap is CORRECT, and the old model
 * failed it. Worse, it printed "layer OFF" on a run that would have been
 * visible, which is how a session gets sent back round the same loop.
 */
static int sim_display_lit;

/*
 * Canon's palette, as SelectParameter rebuilds it -- docs/ML_DISPLAY.md 4.6.
 *
 * Every TurnOnDisplay walks the CBR list at MEM(0x00029318) with r0 = 0,
 * reaches NotifyTurnOnDisplayToBitmapDisplayDevice (0xFFB0AC74, registered
 * as a literal at 0xFFB0AEA0), and SelectParameter (0xFFB0A95C) +
 * SetParameterToBitmapDisplayDevice (0xFFB0A888) write Canon's sixteen
 * words over ours. Entries whose alpha byte is <= 0x3F come out of
 * ConvertYUVPaletteToPBPalette as 0x00FC0000 -- transparent (0xFFB0A568).
 *
 * Nothing modelled that, so a payload which writes the palette once passed
 * here and came back invisible. These values are deliberately NOT ML's and
 * mostly transparent, which is the shape of the hazard.
 */
static void sim_seed_palette_regs(void)
{
    int i;
    for (i = 0; i < 16; i++) {
        MEM(CAM_LCD_PAL_REG(i)) = CAM_PALETTE_BASE + (uintptr_t)i * 4;
        MEM(CAM_LCD_PAL_REG2(i)) = CAM_PALETTE_BASE + 0x40u + (uintptr_t)i * 4;
    }
}

static void sim_canon_restores_palette(void)
{
    int i;
    /*
     * Canon rebuilds the LCD_Palette shadows and flushes them. The values
     * are deliberately not the payload's, and mostly the transparent word,
     * which is the failure the reassert exists to catch.
     */
    for (i = 0; i < 16; i++) {
        uint32_t v = (i == 0) ? 0x00FC0000u
                              : (0x01000000u | (uint32_t)(i * 0x0E) << 16);
        uintptr_t reg = (uintptr_t)MEM(CAM_LCD_PAL_REG(i));
        uintptr_t reg2 = (uintptr_t)MEM(CAM_LCD_PAL_REG2(i));
        MEM(CAM_PB_PAL_VAL(i)) = v;
        if (reg) MEM(reg) = v;
        if (reg2) MEM(reg2) = v;
    }
}

/*
 * Power-off, as Canon does it.
 *
 * ml_shutdown_requested goes up, and the GUI tears the bitmap screen down:
 * 0xFFA279B0 calls UndisplayPhysicalScreen (0xFFB0B240), clears the latch
 * at 0xFFA279F4 and then frees the VRAM with DeletePhysicalVram
 * (0xFFB0B0E4). Anything still blitting into that buffer is writing into
 * released heap -- which is why the card LED stayed lit after the camera
 * was switched off.
 *
 * The buffer here is a static array, so it cannot actually be freed; what
 * is modelled is the ORDER, and the checks below count any write that
 * arrives after the latch is clear.
 */
long sim_blits_after_teardown;

static void sim_panel_power_off(void);

static void sim_shutdown(void)
{
    MEM(CAM_ml_shutdown)   = 1;
    MEM(CAM_gui_bmp_ready) = 0;        /* 0xFFA279F4, before the free */
    sim_panel_power_off();
}

/* Canon powering its own panel: what the fake MENU press ends up doing,
 * and what TurnOnDisplay would do if we called it (we do not). */
static void sim_panel_power_on(void)
{
    sim_display_lit = 1;
    MEM(CAM_display_is_on)     = 1;
    MEM(CAM_bmp_vram_real_ptr) = (uintptr_t)sim_bmp_real;
    /* TurnOnDisplay 0xFFB0933C: re-apply the recorded layer state. It is
     * 0xEA and there is no code anywhere that makes it anything else. */
    sim_bitmap_shown = (MEM(CAM_bmp_shown) == 0xEA);
    sim_canon_restores_palette();
}

static void sim_panel_power_off(void)
{
    sim_display_lit = 0;
    MEM(CAM_display_is_on) = 0;
    /* The pointer stays published and the layer stays shown -- the camera
     * reported "displayon=0 vram=0x105890C0", and nothing hides the layer. */
}

void sim_turn_on_display(void)
{
    sim_turnons++;                      /* counted so the suite can refuse it */
    if (sim_capture_in_flight) sim_ddev_during_capture++;
    sim_panel_power_on();
}

static void sim_display_init(void)
{
    sim_seed_palette_regs();
    MEM(CAM_display_is_on)      = 0;
    MEM(CAM_bmp_vram_real_ptr)  = 0;
    /* Canon's GUI has built the bitmap screen: CreatePhysicalVram plus
     * DisplayPhysicalScreen, latched at 0xFFA25300 */
    MEM(CAM_gui_bmp_ready)      = 1;
    MEM(CAM_ml_shutdown)        = 0;
    /* The layer, as Canon's one-shot bring-up leaves it for the whole boot */
    sim_bitmap_shown            = 1;
    MEM(CAM_bmp_shown)          = 0xEA;
    sim_canon_restores_palette();
}

/* ML's menu lights it too, which is how the interface reaches the screen
 * today; the redraw model below stands in for that. */
long sim_gui_controls;

/*
 * Canon's event poster, as ML uses it: a faked MENU press, which makes
 * Canon open its own menu and in doing so power the panel and show the
 * bitmap layer -- in Canon's order, by Canon's code.
 *
 * Modelling it this way is the point. A payload that asks for the screen
 * gets it; one that reaches for TurnOnDisplay and ShowBitmap itself gets
 * a simulator that notices.
 */
/*
 * Canon's event poster, as ML uses it: a faked MENU press, which makes
 * Canon open its own menu and in doing so power the panel -- in Canon's
 * order, by Canon's code.
 *
 * TWO THINGS THIS MODEL USED TO GET WRONG, and both hid a real defect.
 *
 * 1. THE EVENT WENT STRAIGHT TO CANON. On the camera it does not. It is
 *    posted to GuiMainTask's message queue at MEM(0x1271C), and
 *    my_gui_task dequeues it and calls OUR hook first -- the `bl` at
 *    0x00989DB4 that patch-film.py replaces. So se_key sees our own fake
 *    press before Canon does, and the last build read param == 3, hit
 *    `case SE_BTN_MENU`, and closed the screen it had just asked to open.
 *    The log has it:
 *
 *      screen: asking Canon for the panel (fake MENU)
 *        ev 00000000 00000003 00000000 FFFE1DC0  t=3231
 *      screen: displayon=0 vram=0x105890C0
 *      screen: the panel went out, closing
 *
 *    0xFFFE1DC0 is FAKE_BTN, from fake_simple_button's literal pool at
 *    0x00988438. The event is now routed through se_key here too, so the
 *    desktop reproduces that instead of passing it.
 *
 * 2. THE PANEL CAME ON INSTANTLY. It does not. ML's menu_open does
 *    `if (!DISPLAY_IS_ON) fake_simple_button(BGMT_MENU); msleep(50);`
 *    (0x0098728C) and then does NOT wait -- and the camera proves 50 ms is
 *    not enough: `screen: displayon=0` was logged after exactly that wait.
 *    ML survives because the next key posts another redraw. A payload with
 *    one trigger has to poll, and an instant model never made it.
 */
/*
 * Canon's GUI state machine is one thing with one state, so these are
 * serialised. Without the lock two fake presses in flight race on the
 * toggle and the second can switch the panel off immediately after the
 * first switched it on -- which is a bug in the model, not in the payload,
 * and it made one run in four look like a failure.
 */
static pthread_mutex_t sim_gui_state = PTHREAD_MUTEX_INITIALIZER;

static void *sim_fake_menu_thread(void *arg)
{
    long code = (long)(intptr_t)arg;
    int ev[4];
    ev[0] = 0;                      /* type: the button class */
    ev[1] = (int)code;              /* param */
    ev[2] = 0;                      /* obj */
    ev[3] = SE_FAKE_BTN;            /* arg -- 0xFFFE1DC0 */

    usleep(5000);                   /* queue latency before GuiMainTask runs */
    if (se_key(ev) == 0) {
        /* consumed: Canon is never asked, and nothing happens. This is
         * exactly what the camera did on 2026-10-21. */
        sim_fakes_eaten++;
        return NULL;
    }
    pthread_mutex_lock(&sim_gui_state);
    usleep(200000);                 /* Canon's own menu bring-up, > 50 ms */
    if (MEM(CAM_display_is_on)) sim_panel_power_off();   /* closing its menu */
    else                        sim_panel_power_on();    /* ...or opening it */
    pthread_mutex_unlock(&sim_gui_state);
    return NULL;
}

void sim_gui_control(long code, long a1, long a2, long a3)
{
    pthread_t th;
    (void)a1; (void)a3;
    sim_gui_controls++;
    if (sim_capture_in_flight) sim_ddev_during_capture++;
    if (code != SE_BGMT_MENU || a2 != SE_FAKE_BTN) return;
    pthread_create(&th, NULL, sim_fake_menu_thread, (void *)(intptr_t)code);
    pthread_detach(th);
}

static void sim_display_menu_opened(void)
{
    if (!sim_display_lit) sim_turn_on_display();
}

/*
 * The payload's stack canary symbol. film.c declares it `extern char
 * __wstack_bottom[]` -- an array, not a pointer -- so it must be defined as
 * one here too, or the canary writes 64 bytes over an 8-byte global.
 */
#define WSTACK_BYTES (48 * 1024)
char __wstack_bottom[WSTACK_BYTES];

static const char *g_root = ".";
static int   g_short_reads = 1;
static int   g_zero_reads = 0;   /* "nothing right now", mid-file */
static int   g_zero_burst = 0;   /* ...this many times running */
static int   g_lazy_writes = 1;  /* the card finishes the tail later */
static int   g_verbose = 0;

/* ------------------------------------------------------------- stats */
static long  st_reads, st_short_reads, st_bytes_read, st_writes, st_bytes_written;
static size_t st_alloc_live, st_alloc_peak;

static const unsigned char *g_pend_buf;
static void sim_settle(void);

/* ------------------------------------------------------------- heap */
/*
 * Magic Lantern leaves Canon's heap as [0x280000, 0x980000) on this body,
 * so 7 MB, and some of that is already spoken for. Refusing anything that
 * would not fit there is the whole point: it is how an allocation the
 * camera could never satisfy gets caught here instead.
 */
/*
 * Not 7 MB. Magic Lantern leaves Canon's heap spanning 0x280000..0x980000,
 * but most of it is already spoken for: the camera reported 2264 KB free
 * when a develop asked for 2.2 MB and was refused. Modelling the real figure
 * is the point -- an allocation the body cannot satisfy has to fail here
 * too, or the simulator signs off on something that cannot run.
 */
#define SIM_HEAP_BYTES (7u * 1024 * 1024)
#define SIM_HEAP_INUSE (SIM_HEAP_BYTES - 2264u * 1024)

void *cam_alloc(size_t n)
{
    void *p;
    if (st_alloc_live + n + SIM_HEAP_INUSE > SIM_HEAP_BYTES) {
        if (g_verbose) fprintf(stderr, "[sim] REFUSED alloc of %zu (live %zu)\n",
                               n, st_alloc_live);
        return NULL;
    }
    p = malloc(n + 16);
    if (!p) return NULL;
    *(size_t *)p = n;
    if (g_verbose)
        fprintf(stderr, "[sim] alloc %7zu  (live %zu KB)\n", n, (st_alloc_live + n) >> 10);
    st_alloc_live += n;
    if (st_alloc_live > st_alloc_peak) st_alloc_peak = st_alloc_live;
    return (char *)p + 16;
}

void cam_free(void *p)
{
    char *b;
    size_t n;
    if (!p) return;
    b = (char *)p - 16;
    n = *(size_t *)b;
    /*
     * A write may still be pending out of this block. On the camera the card
     * would read freed memory; here it would read freed memory too, so
     * settle it first -- and say so, because code that frees a buffer with a
     * write outstanding is wrong on the body as well.
     */
    if (g_pend_buf && (const unsigned char *)g_pend_buf >= (unsigned char *)p &&
        (const unsigned char *)g_pend_buf < (unsigned char *)p + n) {
        fprintf(stderr, "[sim] WARNING: freed a buffer with a write pending\n");
        sim_settle();
    }
    st_alloc_live -= n;
    free(b);
}

void cam_meminfo(int *total, int *freebytes)
{
    if (total) *total = (int)SIM_HEAP_BYTES;
    if (freebytes) *freebytes = (int)(SIM_HEAP_BYTES - SIM_HEAP_INUSE - st_alloc_live);
}

/* ------------------------------------------------------------- files */
/*
 * The payload starts the watcher from inside se_task and then carries on
 * logging, so two threads really do use this file layer at once -- as two
 * tasks do on the camera. The mock has to be safe for that or it invents
 * crashes of its own.
 */
#define SIM_MAX_FH 16
static FILE *g_fh[SIM_MAX_FH];
/* card calls made after Canon asked to shut down -- see the power-off
 * check at the end of the run */
static long g_fio_after_shutdown;
long sim_fio_calls_since_shutdown(void) { return g_fio_after_shutdown; }

static pthread_mutex_t g_fio = PTHREAD_MUTEX_INITIALIZER;
#define FIO_LOCK()   pthread_mutex_lock(&g_fio)
#define FIO_UNLOCK() pthread_mutex_unlock(&g_fio)

/* "A:/ML/SE.BIN" -> "<root>/ML/SE.BIN" */
static void sim_path(const char *in, char *out, size_t n)
{
    const char *p = in;
    if ((p[0] == 'A' || p[0] == 'B') && p[1] == ':') p += 2;
    while (*p == '/') p++;
    snprintf(out, n, "%s/%s", g_root, p);
    /* strip a trailing slash, which Canon's opendir tolerates */
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
}

static int sim_slot(void)
{
    int i;
    for (i = 0; i < SIM_MAX_FH; i++) if (!g_fh[i]) return i;
    return -1;
}

/*
 * Every call into the filesystem, counted.
 *
 * Not for statistics: the button hook runs on Canon's GUI task, on a stack
 * the entry stub deliberately does not switch, and a Canon filesystem call
 * is far more than that stack is sized for. Two builds did one in the
 * DELETE path and both came back from the camera with every button dead;
 * the build between them, which did not, worked. So the rule is checked --
 * se_key may not touch the card, at all, ever.
 */
long sim_fio_calls;

int cam_open(const char *path)
{
    sim_fio_calls++;
    if (MEM(CAM_ml_shutdown)) g_fio_after_shutdown++;
    char rp[1024]; int s;
    FIO_LOCK();
    s = sim_slot();
    if (s < 0) { FIO_UNLOCK(); return -1; }
    sim_path(path, rp, sizeof rp);
    g_fh[s] = fopen(rp, "rb");
    if (!g_fh[s]) s = -1;
    FIO_UNLOCK();
    return s;
}

int cam_create(const char *path)
{
    sim_fio_calls++;
    if (MEM(CAM_ml_shutdown)) g_fio_after_shutdown++;
    char rp[1024]; int s;
    FIO_LOCK();
    s = sim_slot();
    if (s < 0) {
        fprintf(stderr, "[sim] cam_create: no free handles\n");
        FIO_UNLOCK(); return -1;
    }
    sim_path(path, rp, sizeof rp);
    g_fh[s] = fopen(rp, "wb+");
    if (!g_fh[s]) {
        fprintf(stderr, "[sim] cam_create FAILED: %s (slot %d)\n", rp, s);
        s = -1;
    }
    FIO_UNLOCK();
    return s;
}

/*
 * The important one. Canon's FIO_ReadFile is under no obligation to return
 * everything asked for, and assuming it did corrupted every frame the camera
 * made. Here it deliberately returns a partial result most of the time.
 */
int cam_read(int h, void *buf, size_t n)
{
    size_t want = n, got;
    if (h < 0 || h >= SIM_MAX_FH || !g_fh[h]) return -1;
    FIO_LOCK();
    st_reads++;
    /*
     * A storage layer is allowed to say "nothing available" without meaning
     * end of file. Code that treats 0 as EOF then silently truncates -- which
     * is what left stock files short of their NAME line on the camera, and a
     * different file failed on each run depending on where the boundary fell.
     */
    /*
     * A single zero read is the easy case: read_at retries and gets on with
     * it. The one that matters is a *burst* -- a card that says "nothing
     * right now" several times in a row, for longer than the caller is
     * willing to retry. That is what a real card does when it is busy, and
     * it is the only way to find out what the engine does when a read
     * genuinely gives up partway through a frame.
     */
    if (g_zero_burst && (st_reads % 503) > (503 - g_zero_burst)) {
        st_short_reads++; FIO_UNLOCK(); return 0;
    }
    if (g_zero_reads && (st_reads % 7) == 3) { st_short_reads++; FIO_UNLOCK(); return 0; }
    if (g_short_reads && n > 512) {
        /* a prime-ish chunk so boundaries land in awkward places */
        size_t cap = 1543 + (size_t)(st_reads * 977) % 4096;
        if (cap < want) { want = cap; st_short_reads++; }
    }
    got = fread(buf, 1, want, g_fh[h]);
    st_bytes_read += (long)got;
    FIO_UNLOCK();
    return (int)got;
}

/*
 * Canon's FIO_WriteFile returns before the data has left the caller's
 * buffer. The first camera frame that decoded correctly still came out
 * banded, and diffing it against the desktop put all 318 wrong bytes in the
 * last 28 of a 4096-byte block: the tail of each write was overwritten while
 * the card was still reading it.
 *
 * So this models it. The head of the write goes out now; the tail is left
 * pending and only read out of the caller's buffer when the *next* write
 * happens -- by which time correct code has left it alone, and incorrect
 * code has scribbled on it.
 */
#define SIM_LAZY_TAIL 28
static size_t g_pend_len;
static int    g_pend_fh = -1;

static void sim_settle(void)
{
    if (g_pend_fh >= 0 && g_pend_len && g_fh[g_pend_fh]) {
        fwrite(g_pend_buf, 1, g_pend_len, g_fh[g_pend_fh]);
    }
    g_pend_fh = -1; g_pend_len = 0; g_pend_buf = NULL;
}

int cam_write(int h, const void *b, size_t n)
{
    size_t head = n;
    if (h < 0 || h >= SIM_MAX_FH || !g_fh[h]) return -1;
    FIO_LOCK();
    sim_settle();
    st_writes++;
    st_bytes_written += (long)n;
    if (g_lazy_writes && n > SIM_LAZY_TAIL) {
        head = n - SIM_LAZY_TAIL;
        g_pend_buf = (const unsigned char *)b + head;
        g_pend_len = SIM_LAZY_TAIL;
        g_pend_fh  = h;
    }
    fwrite(b, 1, head, g_fh[h]);
    FIO_UNLOCK();
    return (int)n;
}

void cam_close(int h)
{
    FIO_LOCK();
    if (g_pend_fh == h) sim_settle();
    if (h >= 0 && h < SIM_MAX_FH && g_fh[h]) { fclose(g_fh[h]); g_fh[h] = NULL; }
    FIO_UNLOCK();
}

void cam_remove(const char *p)
{
    char rp[1024]; sim_path(p, rp, sizeof rp); remove(rp);
}

int cam_seek(int h, int pos, int whence)
{
    int r;
    if (h < 0 || h >= SIM_MAX_FH || !g_fh[h]) return -1;
    FIO_LOCK();
    r = 0;
    if (fseek(g_fh[h], pos, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END))
        r = -1;
    else r = (int)ftell(g_fh[h]);
    FIO_UNLOCK();
    return r;
}

int sim_get_file_size(long path, long out)
{
    sim_fio_calls++;
    if (MEM(CAM_ml_shutdown)) g_fio_after_shutdown++;
    char rp[1024]; struct stat sb;
    sim_path((const char *)path, rp, sizeof rp);
    if (stat(rp, &sb) != 0) return -1;
    *(uint32_t *)out = (uint32_t)sb.st_size;
    return 0;
}

/* ------------------------------------------------------- directories */
/*
 * Mirrors what the ROM actually does, read off its disassembly: the cookie
 * is a heap pointer, failure is reported as the small integers 1 and 5, the
 * attribute is a halfword at +0 and the name is a NUL-terminated string at
 * +0x0C.
 */
struct sim_dir { DIR *d; char path[1024]; };

static void sim_fill(long file, const char *dir, const char *name)
{
    char full[2048]; struct stat sb;
    uint8_t *f = (uint8_t *)file;
    snprintf(full, sizeof full, "%s/%s", dir, name);
    memset(f, 0, 16);
    if (stat(full, &sb) == 0) {
        *(uint16_t *)(f + 0) = S_ISDIR(sb.st_mode) ? 0x10 : 0x20;
        *(uint32_t *)(f + 4) = (uint32_t)sb.st_size;
    }
    snprintf((char *)(f + 16), 256, "%s", name);
}

long sim_find_first(long dir, long file)
{
    char rp[1024];
    struct sim_dir *s;
    struct dirent *e;
    if (!file) return 1;
    sim_path((const char *)dir, rp, sizeof rp);
    s = calloc(1, sizeof *s);
    s->d = opendir(rp);
    if (!s->d) { free(s); return 1; }
    snprintf(s->path, sizeof s->path, "%s", rp);
    while ((e = readdir(s->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        sim_fill(file, s->path, e->d_name);
        return (long)s;
    }
    closedir(s->d); free(s);
    return 1;
}

int sim_find_next(long cookie, long file)
{
    struct sim_dir *s = (struct sim_dir *)cookie;
    struct dirent *e;
    if (!file) return 1;
    while ((e = readdir(s->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        sim_fill(file, s->path, e->d_name);
        return 0;
    }
    return 1;
}

void sim_find_close(long cookie)
{
    struct sim_dir *s = (struct sim_dir *)cookie;
    if (!s) return;
    closedir(s->d); free(s);
}

/* ------------------------------------------------------------- misc */
void cam_msleep(int ms) { usleep((useconds_t)ms * 200); }   /* 5x faster */
/*
 * VxWorks' 100 Hz tick.
 *
 * This used to return a counter that advanced by a whole simulated second
 * on every call, which made anything that measures an interval meaningless:
 * the payload's "was the menu on screen recently" test could never be true
 * here, because one intervening call aged it by a second. On the body the
 * gap between a redraw and a button press is milliseconds.
 *
 * It runs on the same compressed scale as cam_msleep above -- five
 * simulated seconds to the real one -- because the payload both sleeps and
 * reads the clock, and two clocks running at different rates would be a
 * worse lie than one running fast. On this scale cam_msleep(1000) really
 * does advance this by 100.
 */
#define SIM_TIME_SCALE 5

long sim_tick_get(void)
{
    struct timespec ts;
    static long base = -1;
    long now;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = ((long)ts.tv_sec * 100 + ts.tv_nsec / 10000000L) * SIM_TIME_SCALE;
    if (base < 0) base = now;
    return now - base;
}

void cam_menu_icon(int x, int y, int t, int a) { (void)x;(void)y;(void)t;(void)a; }

/*
 * Are the palette registers ours right now?
 *
 * Against se_palette[], the payload's own table -- not against ML's image.
 * The palette stopped being ML's when the interface stopped looking like a
 * debug overlay: ML's sixteen bold primaries have no off-white in them.
 * This check is "does the hardware hold what the payload intends", and
 * there is no copy involved because it reads the payload's actual array.
 *
 * Exactly one word still has to agree with ML's image, and the check for
 * that is separate and at the end of the run: entry 15 is the word
 * set_ml_palette_if_dirty compares at 0x00981C6C to decide whether to put
 * ML's palette back over ours.
 */
static int sim_palette_is_ours(void)
{
    int i;
    for (i = 0; i < ML_PALETTE_N; i++)
        if (MEM(CAM_PALETTE_BASE + (uintptr_t)i * 4) != se_palette[i])
            return 0;
    return 1;
}

void cam_memcpy(volatile void *dst, const void *src, size_t n)
{
    /*
     * A frame reaching the panel through a palette that is not ours is
     * invisible, or nearly so, and every counter says success -- which is
     * the whole of defect D2 in docs/ML_DISPLAY.md 10.5. Catch it here,
     * where it costs nothing.
     */
    /*
     * Only the FIRST row of a frame is judged.
     *
     * The payload blits row by row now, the way bmp_idle_copy(1, 0) does
     * (0x00980B64), so one frame is 240 calls through here. Judging each of
     * them counts the same frame 240 times -- and worse, it turns a panel
     * that goes out *during* a blit into dozens of violations, which is a
     * race between the check and the copy rather than a defect. The first
     * row, `dst == sim_bmp_real`, is the moment the frame started, and that
     * is what the preconditions are about.
     *
     * Writing into the buffer after the panel has gone out mid-frame is
     * harmless -- it is invisible, which is the symptom, not a fault.
     */
    if ((const uint8_t *)dst == sim_bmp_real) {
        /* after Canon has cleared the latch the buffer is about to be
         * freed; a frame arriving here is a write into released heap */
        if (MEM(CAM_ml_shutdown))
            sim_blits_after_teardown++;
        /*
         * P4 / defect D2: Canon's palette is back and ours is not, so this
         * frame would be partly or wholly transparent.
         */
        if (!sim_palette_is_ours())  sim_blits_bad_palette++;
        /*
         * P1, docs/ML_DISPLAY.md 6.5: five separate places in ML's own path
         * refuse to do anything while MEM(0x0000FCA0) is clear --
         * menu_redraw_do 0x0098668C, set_ml_palette 0x00981BD4,
         * set_ml_palette_if_dirty 0x00981C60, EngDrvOut 0x00989134 and
         * ShowBitmap 0xFFB09820. A frame started into a dark panel is the
         * first failure this project had, and the build that preceded this
         * one noted the flag without obeying it.
         */
        if (!MEM(CAM_display_is_on)) sim_blits_panel_dark++;
    }
    memcpy((void *)dst, src, n);
}

/* The payload asks for a task; run it on a thread with the real stack size. */
static void *(*g_entry)(void *);
static void *thread_trampoline(void *arg)
{
    void (*fn)(void) = (void (*)(void))arg;
    fn();
    return NULL;
}
void sim_task_create(long name, long prio, long stack, long entry, long arg)
{
    pthread_t th; pthread_attr_t at;
    (void)prio; (void)stack; (void)arg;
    printf("[sim] task_create(\"%s\")\n", (const char *)name);
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512 * 1024);
    pthread_create(&th, &at, thread_trampoline, (void *)entry);
    pthread_detach(th);
}

/* ------------------------------------------- ML's menu redraw, reproduced */
/*
 * The payload no longer rewrites any of Magic Lantern's code. It installs a
 * display callback into the Debug entries and lets ML call it, which is why
 * this has to reproduce how ML calls it -- faithfully enough that an offset
 * four bytes out fails here instead of on the body.
 *
 * menu_redraw_do (0x00986668) does, in this order:
 *
 *     if (!MEM(0x0000FCA0)) return;          DISPLAY_IS_ON
 *     bmp_draw_to_idle(1);                   MEM(0x009C4068) = 1
 *     bmp_fill(COLOR_BLACK, 0, 40, 720, 400);
 *     ... menus_display(0x00985A8C) ...
 *     bmp_draw_to_idle(0);                   MEM(0x009C4068) = 0
 *     bmp_idle_copy(1, 0);                   idle -> real, all 86,400 bytes
 *
 * and menus_display, per entry:
 *
 *     if ((int8_t)[e+0x30] == 1) skip;       hidden, and not counted
 *     ip = [e+0x2C];
 *     if (ip) ip([e+0x0C], x, y, (uint8_t)[e+0x08]);
 *     else    ML's own row renderer;
 *     if (++rows >= limit) break;            limit is 10 or 11
 *     e = [e+0x00];
 *
 * Both of those matter to us. The idle flag is the reason anything drawn
 * from a task of our own is invisible: it lands in the real buffer and the
 * copy at the end wipes it. The hidden flag and the row limit are two quiet
 * ways for our row to be dropped without any error anywhere.
 */
#define SIM_MENU_ROW_LIMIT 10          /* the smaller of the two ML uses */
#define SIM_MENU_X         40
#define SIM_MENU_Y0        45
#define SIM_MENU_DY        27
#define SE_COLS_SIM        37      /* 720 / 20, font 2, plus the NUL */

typedef void (*sim_display_fn)(void *priv, int x, int y, int selected);

static int sim_rows_ours, sim_rows_ml;

/*
 * An address inside ML's relocated image. Pointers that live there belong to
 * the camera and mean nothing on this host: a string there cannot be
 * dereferenced and a function there cannot be called. Telling the two apart
 * is also a check worth having -- if the payload ever installed a camera
 * address where one of its own belongs, it would land here.
 */
#define SIM_ML_END (SIM_ML_BASE + SIM_ML_SIZE)
static int sim_is_ml_addr(uintptr_t a)
{
    return a >= SIM_ML_BASE && a < SIM_ML_END;
}

/* Copy a string out of ML's image, which is words in sim_ml rather than
 * bytes this process can read directly. */
static void sim_str(uintptr_t va, char *out, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n; i++) {
        uintptr_t w = MEM((va + i) & ~(uintptr_t)3);
        char c = (char)((w >> (8 * ((va + i) & 3))) & 0xFF);
        if (!c) break;
        out[i] = c;
    }
    out[i] = '\0';
}

static int sim_check_ml_code_intact(const char *image)
{
    uintptr_t now = MEM(ML_menus_display);
    uintptr_t was = 0;
    FILE *f = fopen(image, "rb");
    unsigned char b[4];

    printf("[sim] --- ML's code ---\n");
    if (f) {
        if (fseek(f, (long)(ML_menus_display - SIM_RESTART + SIM_IMAGE_OFF),
                  SEEK_SET) == 0 && fread(b, 1, 4, f) == 4)
            was = (uintptr_t)b[0] | ((uintptr_t)b[1] << 8) |
                  ((uintptr_t)b[2] << 16) | ((uintptr_t)b[3] << 24);
        fclose(f);
    }
    if (!was) {
        printf("[sim]   skipped: no image to compare against\n");
        return 0;
    }
    if (now != was) {
        printf("[sim]   FAIL menus_display 0x%08X now holds 0x%08lX, "
               "was 0x%08lX -- the payload patched ML's code\n",
               ML_menus_display, (unsigned long)now, (unsigned long)was);
        return 1;
    }
    printf("[sim]   ok   menus_display still holds its own prologue "
           "(0x%08lX); nothing of ML's was rewritten\n", (unsigned long)was);
    return 0;
}

/* ------------------------------------------------------------- driver */
void se_watch(void);
void se_screen(void);
void se_start(void);
extern int se_stock_index;
extern int se_src_index;
extern int se_key(void *ev);

/*
 * The task stubs. On the camera these are the assembly in start.S that
 * switches to the payload's own stack before calling the body; here each
 * thread already has one, so they are just the call.
 *
 * se_boot_start is the payload's entry point -- the word at PAYLOAD+0x04
 * that mlstub's loader branches to. Going in this way rather than calling
 * se_start directly is deliberate: the simulator used to bypass it, which
 * is exactly why it did not notice when that word was zeroed and the
 * camera executed a string pointer.
 */
void se_boot_watch(void)  { se_watch(); }
void se_boot_screen(void) { se_screen(); }
void se_boot_start(void)  { se_start(); }

/*
 * The buttons, and the screen.
 *
 * Canon's event is { type, param, obj, arg } and the hook reads word 1 --
 * confirmed on the camera, where 0xC6539 and 0xDEBD9 came through, which
 * are ML's own HALFSHUTTER sentinels 812345 and 912345. What is NOT
 * confirmed is that a press of DELETE produces a code in the range ML's
 * gui.h calls a button: a whole session of presses produced none.
 *
 * So the payload claims nothing, and the screen is opened directly rather
 * than by a button -- which is what ML/SE.UI does on the camera. Those are
 * two separate questions and they have been failing together.
 */
#define SIM_KEY_WHEEL 0x00
#define SIM_KEY_UP    0x16
#define SIM_KEY_TRASH 0x09
#define SIM_KEY_PLAY  0x08
#define SIM_KEY_SET   0x04       /* platform/50D.109/gui.h BGMT_PRESS_SET */
extern long sim_ddev_takes, sim_ddev_gives, sim_turnons, sim_notifies;
extern long sim_fakes_eaten, sim_blits_bad_palette, sim_blits_panel_dark;
extern const uint32_t se_palette[16];
/*
 * Read from the payload's own state, not from its log.
 *
 * These three facts used to be asserted by grepping ML/SE.LOG, and se_note
 * is allowed to drop a line when it cannot take the log lock -- so all
 * three were flaky, and painting ten frames a second instead of one made
 * them flakier. A missing log line is not evidence of absence (rule 4).
 */
extern volatile int se_dev_from_raw, se_dev_from_preview;
extern volatile uint32_t se_raw_decode_hash[2];
extern volatile uint32_t se_raw_readback_hash;
extern volatile int se_busy;
extern volatile uint32_t se_last_look_hash;
extern volatile int se_request_all;
extern volatile int se_cfg_dirty;
extern const char *se_active_stock_name(void);
extern long sim_shows, sim_hides, sim_gui_controls;
extern int  sim_bitmap_shown;
int  sim_overlay_live;
extern long sim_ddev_during_capture;
extern int  sim_capture_in_flight;

extern volatile int se_scr_blits;
extern volatile int se_scr_want;
extern volatile int se_scr_open;
static int se_scr_open_now(void) { return se_scr_open; }
extern void se_boot_start(void);

static int sim_screen_blits(void)        { return se_scr_blits; }
static void sim_screen_blits_reset(void) { se_scr_blits = 0; }
static void sim_screen_open(int on)      { se_scr_want = on; }

extern long sim_fio_calls;
static long sim_key_fio;           /* card calls made from inside se_key */

static int sim_send_key_arg(int param, int arg)
{
    int ev[4] = { 0, 0, 0, 0 };
    long before = sim_fio_calls;
    int r;
    ev[1] = param;
    ev[3] = arg;
    r = se_key(ev);
    /*
     * Other tasks are running, so this can over-count; it cannot
     * under-count, which is the direction that matters. A hook that never
     * touches the card will show zero here across a quiet moment.
     */
    sim_key_fio += sim_fio_calls - before;
    return r;
}

/* A real press: arg is 0, as the camera logs for every genuine button. */
static int sim_send_key(int param) { return sim_send_key_arg(param, 0); }

/*
 * D1, directly: a fake press must reach Canon whatever state we are in.
 *
 * sim_gui_control already routes the payload's own fake presses through
 * se_key, so a hook that eats them shows up as sim_fakes_eaten -- but only
 * if the payload asks for the panel during the run. This asks the question
 * outright, with the screen open, which is the exact state the last build
 * failed in: se_scr_want set, param 3, and `case SE_BTN_MENU` closing the
 * screen it had just asked to open.
 *
 * The codes tried are the two the engine claims for itself while open:
 * MENU (3, which closed the screen) and TRASH (7, which toggles it).
 */
static int sim_check_fake_presses_pass_through(void)
{
    int bad = 0, want0 = se_scr_want;
    printf("[sim] --- a fake press, with the screen open ---\n");
    se_scr_want = 1;
    if (sim_send_key_arg(SE_BGMT_MENU, SE_FAKE_BTN) == 0 || !se_scr_want) {
        printf("[sim]   FAIL a fake MENU press (arg == FAKE_BTN, 0x%08X) was "
               "consumed, or closed the screen. GUI_Control posts it into "
               "GuiMainTask's queue and my_gui_task hands it to our hook "
               "before Canon sees it, so eating it means Canon is never "
               "asked and the panel never comes on\n", (unsigned)SE_FAKE_BTN);
        bad = 1;
    }
    if (sim_send_key_arg(7, SE_FAKE_BTN) == 0 || !se_scr_want) {
        printf("[sim]   FAIL a fake TRASH press was consumed, or toggled the "
               "screen. ML's 2012 my_gui_task passes arg straight through "
               "(0x00989E54) and so must we\n");
        bad = 1;
    }
    if (!bad)
        printf("[sim]   ok   fake presses pass through and change nothing\n");
    se_scr_want = want0;
    return bad;
}

static int sim_check_key_touches_no_card(void)
{
    printf("[sim] --- the button hook and the card ---\n");
    if (sim_key_fio > 0) {
        printf("[sim]   FAIL %ld filesystem calls were made while se_key "
               "was running. It is on Canon's GUI task, on a stack the "
               "entry stub does not switch -- this is what left two "
               "cameras with no working buttons\n", sim_key_fio);
        return 1;
    }
    printf("[sim]   ok   no filesystem call from the button hook\n");
    return 0;
}

/*
 * TRASH is always ours; the engine's own buttons are ours while it is
 * open; everything else is Canon's, always.
 *
 * The camera showed what not claiming the wheel looks like: the picker
 * drawn on screen with Canon's menu scrolling underneath it. What it also
 * showed, three builds earlier, is what claiming everything looks like --
 * a camera with no working controls. So PLAY stays Canon's and the run
 * fails if it is ever taken.
 */
static int sim_check_keys_closed(void)
{
    int stock = se_stock_index, src = se_src_index, bad = 0;
    printf("[sim] --- buttons with the screen closed ---\n");
    if (sim_send_key(SIM_KEY_WHEEL) != 1 || sim_send_key(SIM_KEY_UP) != 1 ||
        sim_send_key(SIM_KEY_PLAY) != 1) {
        printf("[sim]   FAIL an event other than TRASH was claimed -- out "
               "here the wheel is the aperture dial\n");
        bad = 1;
    }
    if (se_stock_index != stock || se_src_index != src) {
        printf("[sim]   FAIL the stock or source moved (%d->%d, %d->%d)\n",
               stock, se_stock_index, src, se_src_index);
        bad = 1;
    }
    if (!bad) printf("[sim]   ok   all three passed to Canon, nothing moved\n");
    return bad;
}

static int sim_check_keys_open(void)
{
    int stock = se_stock_index, src = se_src_index, bad = 0;
    printf("[sim] --- buttons with the screen open ---\n");
    if (sim_send_key(SIM_KEY_WHEEL) != 0 || se_stock_index == stock) {
        printf("[sim]   FAIL the wheel was not taken, so it scrolls "
               "Canon's menu under our own screen\n");
        bad = 1;
    }
    if (sim_send_key(SIM_KEY_UP) != 0 || se_src_index == src) {
        printf("[sim]   FAIL the joystick did not switch the source\n");
        bad = 1;
    }
    if (sim_send_key(SIM_KEY_PLAY) != 1) {
        printf("[sim]   FAIL PLAY was claimed; only the engine's own "
               "buttons may be\n");
        bad = 1;
    }
    if (!bad)
        printf("[sim]   ok   wheel %d->%d, source %d->%d, PLAY left to "
               "Canon\n", stock, se_stock_index, src, se_src_index);
    /*
     * Put the source back where it was found.
     *
     * This test proves the joystick switches the source, and then it used
     * to leave it switched -- so the develop, which runs on the watcher
     * thread and can start after this, sometimes read the flipped value
     * and the raw run developed from the preview. One run in six.
     *
     * The source is the one piece of state in this run that the card
     * chooses (ML/SE.RAW) and that a later assertion depends on, so a test
     * that exercises it has to leave it as it found it.
     */
    se_src_index = src;
    se_stock_index = stock;
    return bad;
}

/*
 * TRASH is taken, and it opens and closes the screen.
 *
 * Taken is what stops Magic Lantern's menu opening, which the camera has
 * now confirmed: 07 arrived, the count went up, the menu stayed shut and
 * every other button kept working.
 *
 * Opening the screen is the one thing added on top of that build. What it
 * must NOT do is reach the display hardware, which is the separate check
 * further down -- the palette, the lock and TurnOnDisplay are all still
 * left out, and go back one at a time.
 */
/*
 * SET chooses the film. It must not start developing the card.
 *
 * It used to set se_request_all, which makes the watcher sweep every frame
 * on the card -- one press with a full card is an hour of work nobody
 * asked for, and nothing warned. This is the regression test for that.
 */
static int sim_check_set_does_not_sweep(void)
{
    int bad = 0, want0 = se_scr_want;
    printf("[sim] --- SET, with the picker open ---\n");
    se_scr_want = 1;
    se_request_all = 0;
    se_cfg_dirty = 0;
    if (sim_send_key(SIM_KEY_SET) != 0) {
        printf("[sim]   FAIL SET was not taken while the picker was open\n");
        bad = 1;
    }
    if (se_request_all) {
        printf("[sim]   FAIL SET asked for every frame on the card to be "
               "developed. It is a film picker; SET chooses the film\n");
        bad = 1;
    }
    if (!se_cfg_dirty) {
        printf("[sim]   FAIL SET did not mark the choice to be saved, so it "
               "would not survive a power cycle\n");
        bad = 1;
    }
    if (se_scr_want) {
        printf("[sim]   FAIL SET did not close the picker\n");
        bad = 1;
    }
    if (!bad)
        printf("[sim]   ok   SET chose the film, saved it and closed\n");
    se_scr_want = want0;
    return bad;
}

static int sim_check_trash_toggles(void)
{
    int bad = 0;
    printf("[sim] --- TRASH ---\n");
    if (se_scr_want) { printf("[sim]   FAIL already open\n"); return 1; }
    if (sim_send_key(SIM_KEY_TRASH) != 0 || !se_scr_want) {
        printf("[sim]   FAIL TRASH did not take and open the screen -- "
               "ML's menu would come up instead\n");
        bad = 1;
    }
    if (sim_send_key(SIM_KEY_TRASH) != 0 || se_scr_want) {
        printf("[sim]   FAIL TRASH did not close it again\n");
        bad = 1;
    }
    if (!bad) printf("[sim]   ok   open, closed, and taken both times\n");
    return bad;
}

static void sim_show_panel(const char *dump)
{
    int r, c, i;
    long hist[256];
    int bg = 0;
    long best = -1;

    for (i = 0; i < 256; i++) hist[i] = 0;
    for (i = 0; i < ML_BMP_BYTES; i++) hist[sim_bmp_real[i]]++;
    for (i = 0; i < 256; i++) if (hist[i] > best) { best = hist[i]; bg = i; }

    if (dump) {
        FILE *f = fopen(dump, "wb");
        if (f) {
            fwrite(sim_bmp_real, 1, ML_BMP_BYTES, f);
            fclose(f);
            printf("[sim]   wrote %s (tools/fb2png.py renders it)\n", dump);
        }
    }

    printf("[sim]   the panel, one char per 8x4 bytes, background is 0x%02X\n",
           bg);
    for (r = 0; r < ML_BMP_ROWS; r += 4) {
        char line[ML_BMP_PITCH / 8 + 1];
        int any = 0;
        for (c = 0; c + 8 <= ML_BMP_PITCH; c += 8) {
            int j, lit = 0;
            for (j = 0; j < 4; j++)
                for (i = 0; i < 8; i++)
                    if (sim_bmp_real[(r + j) * ML_BMP_PITCH + c + i] != bg) lit++;
            line[c / 8] = lit > 12 ? '#' : lit > 0 ? '.' : ' ';
            if (lit) any = 1;
        }
        line[ML_BMP_PITCH / 8] = '\0';
        if (any) printf("[sim]   y=%3d |%s|\n", r * 2, line);
    }
}

/*
 * Wait until the log on the card is a whole one, and only then leave.
 *
 * se_note rewrites SE.LOG in full on every line -- create, write, close --
 * so for a moment the file is empty. The watcher goes on logging after the
 * develop is finished, so returning from main at the wrong instant kills the
 * process inside that window and leaves a truncated file behind. Whoever
 * reads it next sees lines that were written and then apparently lost, and
 * chases a payload bug that is not there. It cost two rounds of exactly that
 * before being pinned down.
 *
 * A complete file is non-empty and ends in a newline, because that is how
 * se_note always leaves it. Two identical reads in a row means nothing is in
 * flight. This has to be the last thing main does -- checking earlier and
 * then carrying on reopens the window, which is what the first attempt did.
 */
static void sim_wait_log_whole(const char *root)
{
    char path[1024];
    int tries;
    long prev = -1;

    snprintf(path, sizeof path, "%s/ML/SE.LOG", root);
    (void)0;
    for (tries = 0; tries < 400; tries++) {
        char alt[1024];
        FILE *f;
        long n = 0, n2 = 0;
        int last = 0;
        snprintf(alt, sizeof alt, "%s/ML/SE.LO2", root);
        f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); n = ftell(f);
            if (n > 0) { fseek(f, -1, SEEK_END); last = fgetc(f); }
            fclose(f);
        }
        f = fopen(alt, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); n2 = ftell(f);
            if (n2 > n) { fseek(f, -1, SEEK_END); last = fgetc(f); n = n2; }
            fclose(f);
        }
        if (n > 0 && last == '\n' && n == prev) return;
        prev = n;
        usleep(20000);
    }
}

int main(int argc, char **argv)
{
    int want = (argc > 2) ? atoi(argv[2]) : 1;
    const char *e = getenv("SE_SIM_SHORT_READS");
    const char *image = getenv("SE_SIM_AUTOEXEC");
    int waited = 0, pass, bad = 0;
    long onscreen[3];

    setvbuf(stdout, NULL, _IOLBF, 0);      /* keep output if we crash */
    g_root = (argc > 1) ? argv[1] : ".";
    if (e) g_short_reads = atoi(e);
    if (getenv("SE_SIM_ZERO_READS")) g_zero_reads = atoi(getenv("SE_SIM_ZERO_READS"));
    if (getenv("SE_SIM_ZERO_BURST")) g_zero_burst = atoi(getenv("SE_SIM_ZERO_BURST"));
    if (getenv("SE_SIM_LAZY_WRITES")) g_lazy_writes = atoi(getenv("SE_SIM_LAZY_WRITES"));
    if (getenv("SE_SIM_VERBOSE")) g_verbose = 1;
    if (getenv("SE_SIM_STOCK")) se_stock_index = atoi(getenv("SE_SIM_STOCK"));
    /* Default matches PLATFORM=50D.109. Override with SE_SIM_AUTOEXEC or
     * point at vendor/magiclantern-<body>/autoexec.bin for another platform. */
    if (!image) image = "vendor/magiclantern-50D/autoexec.bin";

    printf("[sim] card=%s short_reads=%s zero_reads=%s lazy_writes=%s heap=%u MB\n",
           g_root, g_short_reads ? "ON" : "off", g_zero_reads ? "ON" : "off",
           g_lazy_writes ? "ON" : "off", SIM_HEAP_BYTES >> 20);

    /*
     * ML's region, as shipped. Without the real image the Debug entry list
     * would be whatever this file invented, and a wrong offset would be
     * confirmed rather than caught -- which is exactly what happened last
     * time. A run without it still exercises the develop, but it cannot say
     * anything about the menu, so it says so.
     */
    if (!sim_seed_ml_image(image)) {
        printf("[sim] FATAL: could not read ML's image from %s. The menu "
               "checks need it; point SE_SIM_AUTOEXEC at it.\n", image);
        return 4;
    }
    printf("[sim] ML's image seeded from %s\n", image);
    sim_display_init();
    sim_devstate_init();

    /*
     * In through the same door the camera uses.
     *
     * mlstub's loader ends with `blx PAYLOAD+0x04`, and this stands in for
     * the branch that word holds. The simulator used to call the task
     * starter directly, which is precisely why it did not notice when that
     * header word was zeroed and the camera executed a string pointer. The
     * patch tool checks the word is a branch; this checks the thing it
     * branches to is the thing that starts the engine.
     */
    se_boot_start();

    bad |= sim_check_ml_code_intact(image);

    /*
     * Wait for the watcher to *finish* `want` frames, not merely to create
     * the files -- the log line is written after develop_one returns, so it
     * is the only honest completion signal. Polling for the JPEG's existence
     * caught it mid-write and made every run look truncated.
     */
    /*
     * The screen is open while the develop runs.
     *
     * This is the situation the camera is actually in: the interface shows
     * a progress bar, so the screen task is painting ten times a second
     * while the watcher is in the middle of a develop. Two tasks of ours in
     * flight at once, sharing whatever they share -- which is what the
     * simulator exists to run.
     */
    sim_send_key(SIM_KEY_TRASH);
    while (waited < 1200) {
        char path[1024], line[512]; FILE *p; int done = 0;
        snprintf(path, sizeof path, "%s/ML/SE.LOG", g_root);
        p = fopen(path, "rb");
        if (p) {
            while (fgets(line, sizeof line, p))
                if (strstr(line, " rc=")) done++;
            fclose(p);
        }
        if (done >= want) break;
        usleep(50000); waited++;
    }

    sim_send_key(SIM_KEY_TRASH);        /* shut again before the checks */
    /*
     * And wait for the screen task to actually notice. Closing is a flag
     * the task polls, so it is not shut the instant the button is pressed
     * -- and resetting the panel state underneath a screen that is still
     * open means the next open is not an open at all, and every check
     * downstream measures the wrong thing.
     */
    {
        /*
         * Wait for the screen task to actually shut, not merely for the
         * blits to pause. With the screen closed the count stops moving
         * immediately, so a loop that waits for it to settle exits before
         * the close has happened -- and then the panel state is reset
         * underneath a screen that is still open, and every check after
         * it measures the wrong thing. That was one run in four.
         */
        int settle;
        for (settle = 0; settle < 80; settle++) {
            if (!se_scr_want && !se_scr_open_now()) break;
            usleep(25000);
        }
        usleep(300000);
    }
    sim_screen_blits_reset();

    /*
     * The screen comes AFTER the develop, not before it.
     *
     * On the camera the panel is dark while the engine starts up, which is
     * the state the screen task has to cope with: nothing to draw into
     * until it turns the display on itself.
     */

    /*
     * The screen, which is now entirely ours: TRASH opens it, the screen
     * task lights the panel and paints, TRASH closes it again and the
     * painting stops.
     *
     * Three things are worth failing on, and they are the three that the
     * old arrangement -- drawing from inside ML's menu redraw -- could not
     * even ask about:
     *
     *   nothing is drawn before the button is pressed. A soup engine that
     *   paints over the camera's own screen uninvited is worse than no
     *   soup engine.
     *
     *   the frame reaches the overlay buffer once it is open, through the
     *   payload's own pointer check rather than past it.
     *
     *   it stops when closed. A task that keeps blitting after the screen
     *   is dismissed would fight Canon for the panel forever.
     */
    printf("[sim] --- the screen, opened and closed directly ---\n");
    {
        int before, opened, after, waited2;

        /*
         * Canon's display, lit by Canon.
         *
         * The payload does not call TurnOnDisplay any more -- that is one
         * of the three things being added back one at a time -- so on the
         * camera it relies on the screen already being on, which it is
         * when somebody is looking at it to press a button. The simulator
         * has to do the same or the panel stays dark and nothing can be
         * drawn into it.
         */
        /*
         * The panel as the camera presents it before anyone asks: OFF.
         *
         * The 5D Classic has no live view, so the rear LCD is dark most of
         * the time and MEM(0x0000FCA0) reads zero. The pointer stays
         * published, because the camera publishes it while the panel is
         * dark too: "displayon=0 vram=0x105890C0". What it does not do is
         * scan the buffer out.
         *
         * The BITMAP LAYER IS LEFT ON. This block used to clear
         * sim_bitmap_shown and write 0x15 to MEM(0x0000FC9D), modelling
         * docs/FIRMWARE.md 7 -- and 7 is wrong. Nothing on this body ever
         * hides the layer: UndisplayPhysicalScreen (0xFFB0B244) has zero
         * callers, so HideBitmap (0xFFB09864) is never reached, the byte at
         * 0x0000FC9D is set to 0xEA once by the GUI's one-shot bring-up and
         * never to anything else, and TurnOnDisplay re-applies it to the
         * layer register at 0xFFB0933C on every panel-on.
         * docs/ML_DISPLAY.md 4.4, 14.1.
         */
        sim_panel_power_off();

        before = sim_screen_blits();
        if (before != 0) {
            printf("[sim]   FAIL the screen painted %d frames before anyone "
                   "asked for it\n", before);
            bad = 1;
        } else {
            printf("[sim]   ok   nothing drawn until it is asked for\n");
        }

        /*
         * Wait for the payload to be able to draw before asking it to.
         *
         * se_watch allocates the frame buffer and only then scans the
         * card, and se_screen's first guard is that the buffer exists --
         * so a screen opened before the watcher has got that far paints
         * nothing, camsim exits 3, and simtest then overwrites that status
         * with whatever it checks next. That is why one root cause showed
         * up as three different flakes: FAIL(hash) on the preview passes,
         * FAIL(camsim exit 3) on the power-off pass, and a failed config
         * round-trip, together about three runs in ten.
         *
         * A live stock name means the scan finished, which means the
         * buffer was allocated before it. State, not a sleep.
         */
        for (waited2 = 0; waited2 < 2400 &&
                          strcmp(se_active_stock_name(), "(none)") == 0;
             waited2++)
            usleep(25000);

        sim_screen_open(1);
        for (waited2 = 0; waited2 < 400 && sim_screen_blits() < 3; waited2++)
            usleep(10000);
        opened = sim_screen_blits();
        printf("[sim]   opened: %d frames painted, layer %s, panel %s\n",
               opened, sim_bitmap_shown ? "ON" : "OFF",
               MEM(CAM_display_is_on) ? "on" : "off");
        /*
         * The layer must be on -- and on this body it always is, so this
         * is a check on the model as much as on the payload. If it ever
         * fires, either the payload hid the layer (which it must never do)
         * or this file has drifted from docs/ML_DISPLAY.md 4.4 again.
         */
        if (opened > 0 && !sim_bitmap_shown) {
            printf("[sim]   FAIL painted %d frames with the bitmap layer "
                   "off. Nothing on this body hides it -- "
                   "UndisplayPhysicalScreen has no callers -- so either the "
                   "payload called HideBitmap or this model is wrong\n",
                   opened);
            bad = 1;
        }
        if (opened < 3) {
            printf("[sim]   FAIL the screen opened but painted %d frames\n",
                   opened);
            bad = 1;
        }
        sim_show_panel("out/panel_ours.fb");

        sim_screen_open(0);
        usleep(900000);                 /* longer than one paint interval */
        after = sim_screen_blits();
        usleep(400000);
        if (sim_screen_blits() != after) {
            printf("[sim]   FAIL still painting after it was closed "
                   "(%d then %d frames)\n", after, sim_screen_blits());
            bad = 1;
        } else {
            printf("[sim]   ok   closed, and the painting stopped\n");
        }
    }

    bad |= sim_check_trash_toggles();
    sim_screen_open(1);
    usleep(50000);
    bad |= sim_check_keys_open();
    sim_screen_open(0);
    usleep(50000);
    bad |= sim_check_keys_closed();
    bad |= sim_check_set_does_not_sweep();
    bad |= sim_check_fake_presses_pass_through();
    bad |= sim_check_key_touches_no_card();

    if (bad) { sim_wait_log_whole(g_root); return 3; }

    /*
     * A capture, with the screen open over it.
     *
     * This is the case that was never modelled and that cost a camera an
     * Err and a trip: the engine painting the overlay and reasserting the
     * palette while Canon is taking a photograph. What has to hold is that
     * the screen touches no display hardware at all for the duration --
     * the lock, the palette registers and TurnOnDisplay are all off limits
     * once it is open, and a blit into the overlay buffer is the only
     * thing it is allowed to do.
     */
    printf("[sim] --- a capture, with the screen open over it ---\n");
    sim_screen_open(1);
    /*
     * Wait until the screen is actually painting, rather than for a fixed
     * half second.
     *
     * Opening is not instant any more: the payload asks Canon for the
     * panel and then polls, because the camera measured that ML's own
     * 50 ms is not enough (docs/ML_DISPLAY.md 13, C3). A fixed 500 ms
     * window could close before the panel was even up, and then this test
     * ran against a screen that was not painting and proved nothing.
     */
    {
        long seen = sim_screen_blits();
        for (waited = 0; waited < 240; waited++) {
            usleep(25000);
            if (sim_screen_blits() > seen + 2) break;
        }
    }
    {
        long blits0 = sim_screen_blits();
        sim_ddev_during_capture = 0;

        /*
         * A capture, with the screen open over it.
         *
         * Canon fills the imaging buffers here and they are PROT_NONE to
         * everyone else, so if anything of ours follows the state object's
         * pointers during this window the process dies on the spot and the
         * run fails. Reaching the end of this block is the assertion that
         * the payload stayed away from imaging memory while a frame was
         * being taken -- which is what it did not do when the camera
         * answered Err on the shutter.
         *
         * Wait for the watcher to be out of a develop first, so the thing
         * under test is the capture and not a develop that happens to
         * overlap it.
         */
        for (waited = 0; waited < 1200 && se_busy; waited++) usleep(25000);
        sim_capture_in_flight = 1;
        sim_devstate_capture();
        for (waited = 0; waited < 40; waited++) usleep(25000);
        sim_capture_in_flight = 0;

        if (sim_ddev_during_capture) {
            printf("[sim]   FAIL the screen touched the display hardware %ld "
                   "times during a capture -- the lock, the palette "
                   "registers or TurnOnDisplay\n", sim_ddev_during_capture);
            bad = 1;
        } else if ((long)sim_screen_blits() - blits0 <= 0) {
            /*
             * The point of this test is that the screen keeps painting
             * ACROSS a capture without reaching for display hardware. With
             * no frames painted it proves only that a task doing nothing
             * does nothing, and it passed that way once while the panel
             * happened to be dark.
             */
            printf("[sim]   FAIL the screen painted no frames during the "
                   "capture, so this proves nothing about painting over "
                   "one\n");
            bad = 1;
        } else {
            printf("[sim]   ok   no display hardware touched during the "
                   "capture (%ld frames blitted)\n",
                   (long)sim_screen_blits() - blits0);
        }
    }
    sim_screen_open(0);
    usleep(300000);

    /*
     * The BmpDDev semaphore, over the whole session.
     *
     * The payload takes it for one thing only: the sixteen palette stores
     * and the latch, which is what set_ml_palette_if_dirty does under the
     * same semaphore (take 0x00981C78, give 0x00981C88). It writes them
     * once per panel-on now, not once per paint, because Canon only
     * clobbers the palette when the panel comes on -- the dirty test at
     * CAM_PB_PALETTE_DIRTY is what makes a per-paint check cost nothing.
     *
     * So the ceiling is a small multiple of the panel-ons in this run, not
     * of the paints. If it ever tracked the paint count, the palette would
     * be going into Canon's display device ten times a second from a task
     * that has no idea what the imaging path is doing, which is the shape
     * of the one excursion near DIGIC registers that threw an Err.
     */
    printf("[sim]   display device: %ld takes, %ld gives, %ld TurnOnDisplay, "
           "%ld palette writes\n",
           sim_ddev_takes, sim_ddev_gives, sim_turnons, sim_ddev_takes);
    printf("[sim]   overlay notified %ld times\n", sim_notifies);
    if (sim_ddev_takes > 16) {
        printf("[sim]   FAIL the palette is being reasserted far more often "
               "than Canon clobbers it (%ld takes). It should be written "
               "once per panel-on, gated on MEM(0x0002943C) == 0x03EB0000, "
               "the way set_ml_palette_if_dirty gates it at 0x00981C6C\n",
               sim_ddev_takes);
        bad = 1;
    }
    if (sim_ddev_takes != sim_ddev_gives) {
        printf("[sim]   FAIL the BmpDDev semaphore was taken %ld times and "
               "given back %ld. One missing give deadlocks every Canon task "
               "that touches the display\n", sim_ddev_takes, sim_ddev_gives);
        bad = 1;
    }

    /*
     * The layer, and who switched it.
     *
     * It is on for the whole boot on this body and the payload must not
     * touch it either way -- docs/ML_DISPLAY.md 4.4, and 8 for the list of
     * display calls ML refuses to make.
     */
    printf("[sim]   bitmap layer: %ld shows, %ld hides, now %s\n",
           sim_shows, sim_hides, sim_bitmap_shown ? "ON" : "off");
    printf("[sim]   GUI_Control calls: %ld\n", sim_gui_controls);
    if (sim_shows != 0 || sim_hides != 0) {
        printf("[sim]   FAIL the payload called ShowBitmap/HideBitmap "
               "itself (%ld/%ld). ML never does on this body; it fakes a "
               "MENU press and lets Canon do the sequence\n",
               sim_shows, sim_hides);
        bad = 1;
    }
    if (sim_turnons != 0) {
        printf("[sim]   FAIL the payload called TurnOnDisplay itself "
               "(%ld times); same reason\n", sim_turnons);
        bad = 1;
    }
    if (sim_gui_controls < 1) {
        printf("[sim]   FAIL the screen never asked Canon for the panel\n");
        bad = 1;
    }
    if (sim_notifies != 0) {
        printf("[sim]   FAIL NotifyTurnOnDisplayToBitmapDisplayDevice was "
               "called. It is Canon's own TurnOnDisplay callback -- "
               "registered at 0xFFB0AEA0, which is why xref finds no "
               "branch to it -- and calling it makes Canon put Canon's "
               "palette back (SelectParameter, 0xFFB0A95C)\n");
        bad = 1;
    }

    /*
     * D1, docs/ML_DISPLAY.md 10.5: the hook must not swallow our own fake
     * press. GUI_Control posts the event into GuiMainTask's queue and
     * my_gui_task calls our hook before Canon sees it, so a hook that acts
     * on param == 3 cancels the open it just asked for. The camera logged
     * it: "ev 00000000 00000003 00000000 FFFE1DC0".
     */
    printf("[sim]   fake presses: %ld posted, %ld eaten by our own hook\n",
           sim_gui_controls, sim_fakes_eaten);
    if (sim_fakes_eaten != 0) {
        printf("[sim]   FAIL the button hook consumed %ld of the payload's "
               "own fake MENU presses (arg == FAKE_BTN, 0x%08X). Canon is "
               "never asked and the panel never comes on. ML passes them "
               "through -- the 2012 my_gui_task has no IS_FAKE test at all "
               "and hands arg straight to gui_massive_event_loop "
               "(0x00989E54)\n", sim_fakes_eaten, (unsigned)SE_FAKE_BTN);
        bad = 1;
    }

    /*
     * D2, docs/ML_DISPLAY.md 10.5: Canon rebuilds its own sixteen palette
     * words every time the panel is turned on, and some of them are
     * transparent. A blit made before ours are back is invisible.
     */
    if (sim_blits_panel_dark != 0) {
        printf("[sim]   FAIL %ld frames were blitted into a panel that was "
               "not on (MEM(0x0000FCA0) == 0). ML refuses in five separate "
               "places on this path -- menu_redraw_do 0x0098668C, "
               "set_ml_palette 0x00981BD4, set_ml_palette_if_dirty "
               "0x00981C60, EngDrvOut 0x00989134, ShowBitmap 0xFFB09820\n",
               sim_blits_panel_dark);
        bad = 1;
    }
    if (sim_blits_bad_palette != 0) {
        printf("[sim]   FAIL %ld frames were blitted while the palette "
               "registers were not ours. Canon restores its own on every "
               "panel-on (TurnOnDisplay -> the CBR list at 0x00029318 -> "
               "SelectParameter 0xFFB0A95C -> SetParameter 0xFFB0A888), and "
               "entries with alpha <= 0x3F convert to 0x00FC0000, which is "
               "transparent (0xFFB0A568). ML reasserts on EVERY bmp_vram "
               "call (0x00980BC0) and again at 0x00986BC4\n",
               sim_blits_bad_palette);
        bad = 1;
    }

    /*
     * The palette is the payload's own now, not ML's -- it has to be, for
     * an off-white ground that ML's sixteen bold primaries cannot express.
     * So this no longer checks equality. It checks the three things that
     * are not a matter of taste:
     *
     *  - entry 0 transparent. It is what shows through the overlay, and
     *    Canon's own converter returns 0x00FC0000 for it (0xFFB0A568).
     *
     *  - entry 15 EXACTLY ML's 0x03EB0000. Not decoration:
     *    set_ml_palette_if_dirty (0x00981C50) decides whether to overwrite
     *    the palette from this single word's shadow at 0x0002943C
     *    (0x00981C6C). Holding it means ML's test is permanently satisfied
     *    and ML never fights us for the registers. Get this wrong and the
     *    symptom on the camera is colour flicker at the paint rate, which
     *    is not a thing any desktop run would show.
     *
     *  - every other entry opaque, opacity 3. An entry that came out with
     *    opacity 0 would be invisible, and invisible is the symptom this
     *    whole exercise has been chasing.
     */
    {
        int i, bad_pal = 0;
        if (se_palette[0] != 0x00FC0000u) {
            printf("[sim]   FAIL palette entry 0 is 0x%08X, not the "
                   "transparent 0x00FC0000\n", se_palette[0]);
            bad_pal = 1;
        }
        if (se_palette[15] != 0x03EB0000u) {
            printf("[sim]   FAIL palette entry 15 is 0x%08X, not the "
                   "opaque white 0x03EB0000\n", se_palette[15]);
            bad_pal = 1;
        }
        for (i = 1; i < ML_PALETTE_N; i++)
            if ((se_palette[i] >> 24) != 3) {
                printf("[sim]   FAIL palette entry %d is 0x%08X -- opacity "
                       "%u, not 3. It would be drawn and not seen\n",
                       i, se_palette[i], se_palette[i] >> 24);
                bad_pal = 1;
            }
        if (bad_pal) bad = 1;
        else printf("[sim]   ok   palette: 0 transparent, 15 white, "
                    "1..14 opaque\n");
    }

    /*
     * Power-off, with the screen open over it.
     *
     * ITS OWN RUN. Shutdown is irreversible in this model -- the payload
     * stops, by design -- so a run that does it cannot also be the run
     * that checks a develop finished, and bolting it onto the end of
     * every run made the raw pass intermittently produce no JPEG at all.
     * A check that can break the thing it shares a run with is not a
     * check, it is a second source of failures.
     *
     * tools/simtest.sh gives it a pass of its own with SE_SIM_POWEROFF=1.
     *
     * The camera reported the card access LED staying lit after it was
     * switched off, and the reason was that nothing of ours ever looked at
     * ml_shutdown_requested: the watcher kept walking the card and the
     * screen kept blitting ten times a second into a VRAM buffer Canon's
     * teardown frees. Every one of ML's nine tasks stops on that word --
     * menu_redraw_task's loop is `while (!MEM(0x009C84E0))` at 0x00986C8C
     * -- and ours did not.
     */
    if (!getenv("SE_SIM_POWEROFF")) goto skip_poweroff;
    printf("[sim] --- power off, with the screen open ---\n");
    sim_screen_open(1);
    {
        long seen = sim_screen_blits();
        long after;
        for (waited = 0; waited < 240; waited++) {
            usleep(25000);
            if (sim_screen_blits() > seen + 2) break;
        }
        /*
         * Wait for the develop to finish first.
         *
         * Shutdown is irreversible in this model -- once the flag is up
         * the payload stops, by design -- so anything the run still needs
         * has to have happened. The raw run's two bracketing checksums are
         * produced by the develop, and this test used to switch the
         * payload off before it got there: pass 5 failed with an empty
         * readback hash, which is the test breaking the run rather than
         * the payload failing it.
         */
        for (waited = 0; waited < 2400; waited++) {
            if (!se_busy && (se_dev_from_raw + se_dev_from_preview) > 0) break;
            usleep(25000);
        }
        sim_blits_after_teardown = 0;
        sim_shutdown();
        /* give both tasks a generous chance to misbehave */
        for (waited = 0; waited < 60; waited++) usleep(25000);
        after = sim_blits_after_teardown;
        if (after != 0) {
            printf("[sim]   FAIL %ld frames were blitted after Canon cleared "
                   "MEM(0x0000F3A8) and began tearing the bitmap screen "
                   "down. DeletePhysicalVram (0xFFB0B0E4) frees that "
                   "buffer -- this is a write into released heap while the "
                   "camera is powering off\n", after);
            bad = 1;
        } else {
            printf("[sim]   ok   nothing drawn after the teardown began\n");
        }
        if (sim_fio_calls_since_shutdown() > 0) {
            printf("[sim]   FAIL %ld filesystem calls were made after "
                   "ml_shutdown_requested went up; that is what holds the "
                   "card open and leaves its LED lit\n",
                   sim_fio_calls_since_shutdown());
            bad = 1;
        } else {
            printf("[sim]   ok   no card access after shutdown was "
                   "requested\n");
        }
    }

skip_poweroff:

    /*
     * Wait for the watcher to actually finish a develop before reporting
     * anything about it.
     *
     * There was no wait here at all. It worked by accident: the capture
     * test used to sit for up to thirty seconds waiting for the imaging
     * probe to notice a buffer change, and the develop finished somewhere
     * inside that. Deleting the probe deleted the accident, and the run
     * started reporting "developed: 0 from the preview" with no hash on
     * roughly one pass in six -- which simtest read as FAIL(hash) on a
     * develop that had in fact produced a byte-perfect file, just not yet.
     *
     * The dependency was always on state and never on time, which is the
     * same mistake section 13 of docs/ML_DISPLAY.md already records twice.
     * So wait for the state: the develop counters moving, and the watcher
     * no longer busy.
     */
    for (waited = 0; waited < 2400; waited++) {
        if (!se_busy && (se_dev_from_raw + se_dev_from_preview) > 0) break;
        usleep(25000);
    }

    /*
     * Three facts tools/simtest.sh used to grep out of ML/SE.LOG. se_note
     * may drop any line it cannot take the lock for, so all three were
     * flaky assertions on a droppable line. Read the payload's own state.
     */
    /*
     * Which path the frames actually took. se_src_index is no good here --
     * the button tests above flip it -- so the payload counts the develops.
     */
    if (se_last_look_hash)
        printf("[sim]   look hash %08X\n", (unsigned)se_last_look_hash);
    printf("[sim]   developed: %d from raw, %d from the preview\n",
           se_dev_from_raw, se_dev_from_preview);
    /* the remembered look, so simtest can check ML/SE.CFG round-trips */
    printf("[sim]   active stock: %s   source: %s\n",
           se_active_stock_name(), se_src_index ? "RAW" : "JPEG");
    /*
     * The raw path's two bracketing checksums, from the payload's own
     * state. simtest compares them with tools/hashcheck; it used to grep
     * them out of the log, where se_note may drop the line.
     */
    if (se_dev_from_raw > 0)
        printf("[sim]   raw hashes: decode L=%08X R=%08X readback %08X\n",
               (unsigned)se_raw_decode_hash[0],
               (unsigned)se_raw_decode_hash[1],
               (unsigned)se_raw_readback_hash);

    if (bad) { sim_wait_log_whole(g_root); return 3; }

    printf("[sim] reads=%ld (%ld short) %ld MB in | writes=%ld %ld MB out | peak alloc %zu KB | draws=%ld\n",
           st_reads, st_short_reads, st_bytes_read >> 20,
           st_writes, st_bytes_written >> 20, st_alloc_peak >> 10, st_draws);
    sim_wait_log_whole(g_root);
    return 0;
}
