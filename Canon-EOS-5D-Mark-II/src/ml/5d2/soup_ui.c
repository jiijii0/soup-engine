/** \file
 * soup_ui.c - the upstream Soup Engine film picker, on the EOS 5D Mark II.
 *
 * The screen itself is engine/ui.c, unchanged in spirit from the 5D
 * Classic: a 720x240, 16-colour picture of the stock you are about to
 * shoot. This file is only the camera plumbing around it, taken over from
 * the 60D module unchanged except where noted:
 *
 *   OPEN    SET on the shooting screen (the 5D2's SET does nothing there
 *           while C.Fn IV-3 "Assign SET button" is on its default,
 *           "Normal (disabled)"), or SET on Soup Film in ML's menu.
 *   SHOW    The picker lives inside ML's menu, the way arkanoid.mo does: the
 *           menu keeps the panel lit and Canon's display out of the way,
 *           and menu_redraw_blocked stops ML drawing over us.
 *   COLOUR  16 palette entries (0xE0..0xEF, which ML never uses) are loaded
 *           with upstream's own warm palette while the picker is up --
 *           re-asserted every 200 ms, because Canon resets the palette
 *           after a shot's image review -- and put back from Canon's
 *           shadow copy when it closes.
 *   DRAW    The 720x240 picker is scaled into 427 of the bitmap's 480
 *           lines, centred, because the 5D2's 4:3 panel shows the 720
 *           bitmap columns 8/9 as wide as on a 3:2 one (see ui_blit).
 *   KEYS    Either dial or joystick left/right: film. SET: choose it (and
 *           turn Film on). Trash or MENU: close. Half-shutter: close and
 *           shoot.
 *
 * GPL v2.
 */
#include <dryos.h>
#include <module.h>
#include <menu.h>
#include <bmp.h>
#include <zebra.h>
#include <propvalues.h>
#include <gui-common.h>
#include <vram.h>
#include <fio-ml.h>

#include "ui.h"
#include "soup_ui.h"
#include "soup_dev.h"

extern int menu_redraw_blocked;    /* ML: stop menu redraws while set   */
extern int lv;

#define PAL_BASE 0xE0              /* our 16 entries: 0xE0..0xEF        */

/*
 * Upstream's colours, as (opacity << 24) | (Y << 16) | (U << 8) | V.
 *
 * Not the 5D Classic's words: those were tuned by eye on that body's panel
 * and decode as pink paper through the standard conversion. These are
 * computed from upstream's own RGB targets (engine/ui.h) through the Rec.709
 * YUV conversion the DIGIC 4 bitmap layer uses (as modelled by qemu-eos),
 * with Magic Lantern's white level (Y 235), and round-trip to within one
 * level. The opacity byte is taken from the camera's own white at runtime.
 * (Computed for the 60D; the 5D2 is DIGIC 4 too, so expected to match, but
 * not yet seen on a 5D2 panel.)
 */
static const uint32_t ui_pal[16] = {
    0x00000000,  /* 0  CLEAR  (never drawn: the paper covers everything) */
    0x0019FE02,  /* 1  INK    #1E1A17 */
    0x0033FC04,  /* 2  SHADOW #3D362F */
    0x004FFA05,  /* 3  SLATE  #5E554A */
    0x0072F806,  /* 4  MID    #857A6B */
    0x0095F806,  /* 5  EDGE   #ABA092 */
    0x00C8F505,  /* 6  PAPER  #E2D9C4 */
    0x00D9F704,  /* 7  HILITE #F2EBDA */
    0x0044EC26,  /* 8  RUST   #8A3B22 */
    0x0059E738,  /* 9  RED    #C04A2E */
    0x0089CF2A,  /* A  AMBER  #DC8A32 */
    0x00ADC811,  /* B  GOLD   #D9BE4A */
    0x0075E8F4,  /* C  OLIVE  #6B8A4E */
    0x005F0DE3,  /* D  TEAL   #357380 */
    0x005C0619,  /* E  PLUM   #8E5670 */
    0x00EB0000,  /* F  WHITE */
};

static se_ui        ui;
static se_ui_stock  ui_list[SOUP_UI_MAX];
static uint8_t     *ui_fb;                 /* SE_UI_FB_SIZE bytes        */

static volatile int ui_open;               /* picker on screen           */
static volatile int ui_req;                /* 1 open, 2 choose, 3 cancel */
static volatile int ui_dirty;
static int          ui_tick;               /* 20 ms ticks while open     */
static int          ui_view_stock;         /* browsing films mid-develop */
static int          ui_show_done;          /* DEVELOPED page             */
static int          ui_was_busy, ui_last_pct = -1;

static void pal_set(void)
{
    uint32_t op = LCD_Palette[3 * COLOR_WHITE + 2] & 0xFF000000;
    int i;
    for (i = 1; i < 16; i++) {
        int c = PAL_BASE + i;
        EngDrvOut(LCD_Palette[3 * c],         op | ui_pal[i]);
        EngDrvOut(LCD_Palette[3 * c + 0x300], op | ui_pal[i]);
    }
}

static void pal_restore(void)
{
    int i;
    for (i = 1; i < 16; i++) {
        int c = PAL_BASE + i;
        EngDrvOut(LCD_Palette[3 * c],         LCD_Palette[3 * c + 2]);
        EngDrvOut(LCD_Palette[3 * c + 0x300], LCD_Palette[3 * c + 2]);
    }
}

/*
 * The 5D Mark II's panel is 4:3 (CONFIG_4_3_SCREEN): Magic Lantern's 720
 * bitmap columns span a width that holds 640 square pixels, so each column
 * is 8/9 as wide as a row is tall. Drawn row-doubled to 480 lines, as on
 * the 60D's 3:2 panel, the picker came out 11% too narrow for its height.
 * So it is drawn 8/9 as tall instead -- 427 lines, centred -- which gives
 * the shapes their proportions back, with paper above and below.
 */
#define UI_ROWS  ((2 * SE_UI_H * 8 + 4) / 9)      /* 427 of the 480 lines */
#define UI_TOP   ((480 - UI_ROWS) / 2)

static void ui_blit(void)
{
    uint8_t *b = bmp_vram();
    uint8_t paper = PAL_BASE + 6;                  /* ui_pal[6]: PAPER */
    int x, r;
    if (!b) return;
    for (r = 0; r < 480; r++) {
        uint8_t *dst = b + r * BMPPITCH;
        int y = r - UI_TOP;
        const uint8_t *src;
        if (y < 0 || y >= UI_ROWS) { memset(dst, paper, SE_UI_W); continue; }
        src = ui_fb + (y * SE_UI_H / UI_ROWS) * SE_UI_PITCH;   /* nearest row */
        for (x = 0; x < SE_UI_W; x += 2) {
            uint8_t v = src[x >> 1];
            uint8_t lo = v & 15, hi = v >> 4;
            dst[x]     = lo ? PAL_BASE + lo : COLOR_EMPTY;
            dst[x + 1] = hi ? PAL_BASE + hi : COLOR_EMPTY;
        }
    }
}

static void ui_draw(void)
{
    se_ui_fb fb;
    soup_progress p;
    fb.px = ui_fb;
    soup_ui_get_progress(&p);
    ui.title = soup_ui_film_on() ? "SOUP" : "SOUP - FILM OFF";

    /* Upstream's pages: DEVELOPING while frames are in hand, DEVELOPED
     * when a batch finishes with the picker open, the film list otherwise
     * (or when the wheel asks for it mid-develop). */
    if (p.busy && !ui_view_stock) {
        int i;
        ui.screen  = SE_SCREEN_WORKING;
        ui.percent = p.pct;
        ui.stage   = "DEVELOPING";
        ui.done    = p.done;
        ui.total   = p.total;
        for (i = 0; i < (int)sizeof(ui.file) - 1 && p.file && p.file[i]; i++)
            ui.file[i] = p.file[i];
        ui.file[i] = 0;
    } else if (ui_show_done) {
        ui.screen = SE_SCREEN_DONE;
        ui.done = p.done;
        ui.last_seconds = p.last_secs;
    } else {
        ui.screen = SE_SCREEN_STOCK;
    }
    se_ui_draw(&ui, &fb);
    ui_blit();
    ui_dirty = 0;
}

/* Favourites show as "* NAME" (the font's star) and come first. */
static char ui_names[SOUP_UI_MAX][40];

static int ui_fill_list(void)
{
    int i, n = soup_ui_count();
    if (n > SOUP_UI_MAX) n = SOUP_UI_MAX;
    for (i = 0; i < n; i++) {
        snprintf(ui_names[i], sizeof(ui_names[i]), "%s%s",
                 soup_ui_fav(i) ? "* " : "", soup_ui_name(i));
        ui_list[i].name = ui_names[i];
        ui_list[i].desc = soup_ui_desc(i);
    }
    return n;
}

static void ui_do_open(void)
{
    int i, n;

    if (!ui_fb) ui_fb = malloc(SE_UI_FB_SIZE);
    if (!ui_fb) return;
    n = ui_fill_list();
    se_ui_init(&ui);
    se_ui_set_stocks(&ui, ui_list, n);
    ui.stock = soup_ui_current();
    ui.ready = 1;
    ui.hints = "\x01\x02 FILM   SET CHOOSE   INFO FAV   DEL CLOSE";
    ui.hints_work = "\x01\x02 FILM LIST    DEL CLOSE";
    ui.done_note = "SAVED IN THE FILM FOLDER ON THE CARD";
    ui_view_stock = 0;
    ui_show_done = 0;
    {
        soup_progress p;
        soup_ui_get_progress(&p);
        ui_was_busy = p.busy;
        ui_last_pct = p.pct;
    }

    if (!gui_menu_shown()) {
        gui_open_menu();
        for (i = 0; i < 50 && !gui_menu_shown(); i++) msleep(20);
        if (!gui_menu_shown()) return;
    }
    msleep(100);                    /* let ML finish its own first draw */
    menu_redraw_blocked = 1;
    clrscr();
    pal_set();
    ui_open = 1;
    ui_tick = 0;
    ui_draw();
}

static void ui_do_close(int choose, int leave_menu)
{
    if (!ui_open) return;
    ui_open = 0;
    if (choose) soup_ui_choose(ui.stock);
    clrscr();
    pal_restore();
    menu_redraw_blocked = 0;
    if (leave_menu && gui_menu_shown()) gui_stop_menu();
}

/*
 * The badge: a small "FILM 46%" tag at the top right of the shooting
 * screen while a frame develops ("+2" when more are queued), then "FILM
 * DONE" or "FILM FAILED" for a moment. Canon redraws its own screen over
 * it, so it is put back every 400 ms; when it goes, Canon is asked to
 * redraw so no patch is left behind.
 */
static int badge_shown, badge_hold_until, badge_last_ms;
static int badge_was_busy, badge_failed;
static char badge_text[24];

static void badge_paint(const char *txt)
{
    /* a fixed-size orange box, text right-aligned inside it, so a shorter
     * string never leaves pieces of the longer one behind */
    uint32_t f = FONT(FONT_MED, COLOR_BLACK, COLOR_ORANGE);
    int wmax = MAX(bmp_string_width(f, " FILM 100% +99 "),
                   bmp_string_width(f, " FILM FAILED "));
    int w = bmp_string_width(f, txt);
    int x1 = 712 - wmax, x = 712 - w;
    bmp_fill(COLOR_ORANGE, x1, 8, wmax, fontspec_height(f));
    bmp_printf(f, x, 8, "%s", txt);
}

static void badge_step(void)
{
    soup_progress p;
    int now = get_ms_clock(), ev;
    int visible = display_idle() && !gui_menu_shown();

    soup_ui_get_progress(&p);
    ev = soup_ui_take_event();
    if (ev == 2) {
        badge_failed = 1;
        snprintf(badge_text, sizeof(badge_text), " FILM FAILED ");
        badge_hold_until = now + 3000;
    }
    /* the queue just emptied: say so (unless the last word was a failure) */
    if (badge_was_busy && !p.busy && !badge_failed) {
        snprintf(badge_text, sizeof(badge_text), " FILM DONE ");
        badge_hold_until = now + 2000;
    }
    if (p.busy && !badge_was_busy) badge_failed = 0;
    badge_was_busy = p.busy;

    if (p.busy && now >= badge_hold_until) {
        int queued = p.total - p.done - 1;
        /* no "%%" here: DryOS's snprintf takes an argument for it */
        if (queued > 0)
            snprintf(badge_text, sizeof(badge_text), " FILM %d%s +%d ", p.pct, "%", queued);
        else
            snprintf(badge_text, sizeof(badge_text), " FILM %d%s ", p.pct, "%");
    } else if (!p.busy && now >= badge_hold_until) {
        if (badge_shown) { badge_shown = 0; if (visible) redraw(); }
        return;
    }

    if (visible && now - badge_last_ms >= 400) {
        badge_paint(badge_text);
        badge_shown = 1;
        badge_last_ms = now;
    }
}

/*
 * Playback toggle. In Canon's playback, SET swaps the photo on screen for
 * its film version (FILM/xxxCANON/FLM_nnnn.JPG), painted straight into
 * the playback picture; SET again puts Canon's picture back. Any other
 * key puts it back too and then does what it normally does.
 */
static volatile int play_state;        /* 0 off, 1 loading, 2 showing     */
static volatile int play_abort;
static uint8_t     *play_backup;
static int          play_backup_len, play_num;
static uint8_t     *play_vram;
static int          play_msg_until;

static void play_label(const char *t)
{
    uint32_t f = FONT(FONT_MED, COLOR_WHITE, COLOR_BLACK);
    bmp_printf(f, 16, 444, "%s", t);
}

static void play_message(const char *t)
{
    play_label(t);
    play_msg_until = get_ms_clock() + 2000;
}

/* put Canon's picture back; `canon` also asks Canon to redraw its info */
static void play_restore(int canon)
{
    if (play_backup && play_vram && play_backup_len)
        memcpy(play_vram, play_backup, play_backup_len);
    play_state = 0;
    if (canon) redraw();
}

static void play_free(void)
{
    if (play_backup) free(play_backup);
    play_backup = 0;
    play_backup_len = 0;
    play_state = 0;
}

static void play_toggle(void)
{
    char path[96];
    int folder, num, len, rc, t0;
    struct vram_info *vr;

    if (play_state == 2) { play_restore(1); return; }
    if (play_state == 1) return;
    if (!is_pure_play_photo_mode()) return;

    if (soup_play_current(&folder, &num) != 0) {
        /* The 5D2's "photo on screen" address is learned on the camera:
         * each SET here is one step (soup_play_learn in soup.c). Once
         * learned, carry straight on and show this photo's film. */
        char msg[64];
        int learned = soup_play_learn(msg, sizeof(msg));
        play_message(msg);
        if (!learned || soup_play_current(&folder, &num) != 0) return;
    }
    soup_flm_path_in(folder, num, path, sizeof(path));
    if (!is_file(path)) {
        soup_progress p;
        soup_ui_get_progress(&p);
        soup_log("playback: %04d has no film file", num);
        play_message(p.busy ? " FILM: STILL DEVELOPING " : " FILM: NO FILM VERSION OF THIS PHOTO ");
        return;
    }
    vr = get_yuv422_vram();
    if (!vr || !vr->vram || vr->width < 2 || vr->width > 1920 || vr->height > 1080) {
        play_message(" FILM: SCREEN NOT READY ");
        return;
    }
    len = vr->pitch * vr->height;
    if (play_backup_len != len) {
        if (play_backup) free(play_backup);
        play_backup = malloc(len);
        play_backup_len = play_backup ? len : 0;
    }
    if (!play_backup) { play_message(" FILM: NOT ENOUGH MEMORY "); return; }

    play_vram = vr->vram;
    memcpy(play_backup, play_vram, len);
    play_num = num;
    play_abort = 0;
    play_state = 1;
    clrscr();
    play_label(" FILM  LOADING ");

    t0 = get_ms_clock();
    rc = soup_view_flm(path, play_vram, vr->pitch, vr->width, vr->height, &play_abort);
    if (rc == SE_OK && play_state == 1 && !play_abort) {
        soup_log("playback: showing FLM_%04d in %d ms", num, get_ms_clock() - t0);
        play_state = 2;
        clrscr();
        play_label(" FILM   SET: ORIGINAL ");
        return;
    }
    if (play_state != 1) return;          /* a key already restored it */
    if (rc == SOUP_VIEW_CANCELLED) {
        /* Another key arrived mid-load and went on to Canon. If Canon
         * stayed on this photo, put its picture back; if it moved on,
         * it is drawing the next one itself. */
        int f2, n2;
        if (is_pure_play_photo_mode() && soup_play_current(&f2, &n2) == 0 && n2 == play_num)
            play_restore(1);
        else
            play_state = 0;
        return;
    }
    soup_log("view %s failed rc=%d", path, rc);
    play_restore(1);
    play_message(" FILM: COULDN'T SHOW THE FILM VERSION ");
}

static void play_step(void)
{
    if (play_msg_until && get_ms_clock() >= play_msg_until) {
        play_msg_until = 0;
        if (!play_state && is_pure_play_photo_mode()) redraw();
    }
    if (!is_play_mode()) {
        if (play_backup) play_free();
        play_msg_until = 0;
    }
}

static void soup_ui_task(void *unused)
{
    (void)unused;
    for (;;) {
        int r = ui_req;
        soup_iso_tick();             /* a film just loaded sets its ISO */
        if (r) {
            ui_req = 0;
            if (r == 1 && !ui_open) ui_do_open();
            else if (r == 2) ui_do_close(1, 1);
            else if (r == 3) ui_do_close(0, 1);
            else if (r == 4) play_toggle();
            else if (r == 5 && ui_open) {
                /* INFO: favourite on/off; the list reorders, stay on it */
                int at = soup_ui_toggle_fav(ui.stock);
                se_ui_set_stocks(&ui, ui_list, ui_fill_list());
                ui.stock = at;
                ui_dirty = 1;
            }
        }
        if (ui_open) {
            if (!gui_menu_shown()) {
                /* the menu went away under us (power save, playback...) */
                ui_do_close(0, 0);
            } else {
                /*
                 * Hold the screen. Canon rewrites the display palette on
                 * its own schedule -- after a shot's image review it put
                 * its own colours back over ours, which came out as a
                 * green cast -- and Magic Lantern's menu can still draw
                 * its tab bar once after we take over. So the 15 colours
                 * are re-asserted every 200 ms and the whole picker is
                 * repainted every 500 ms, as upstream did on the 5D
                 * Classic ("reasserts the palette before every frame").
                 */
                soup_progress p;
                soup_ui_get_progress(&p);
                if (p.pct != ui_last_pct) { ui_last_pct = p.pct; ui_dirty = 1; }
                if (ui_was_busy && !p.busy) { ui_show_done = 1; ui_view_stock = 0; ui_dirty = 1; }
                if (!ui_was_busy && p.busy) ui_dirty = 1;
                ui_was_busy = p.busy;
                /* the picker reports the result itself; don't repeat it
                 * on the badge once it closes */
                badge_was_busy = p.busy;
                (void)soup_ui_take_event();

                ui_tick++;
                if (ui_tick % 10 == 0) pal_set();
                if (ui_dirty || ui_tick % 25 == 0) ui_draw();
            }
            msleep(20);
        } else {
            badge_step();
            play_step();
            msleep(50);
        }
    }
}

/* ML menu: SET on "Soup Film" opens the picker (the menu is already up). */
void soup_ui_open_from_menu(void)
{
    ui_req = 1;
}

unsigned int soup_ui_keypress(unsigned int key)
{
    if (!ui_open && (play_state || is_pure_play_photo_mode())) {
        /* Canon playback: SET flips between the photo and its film */
        if (is_pure_play_photo_mode()) {
            if (key == MODULE_KEY_PRESS_SET) {
                if (play_state == 2) play_restore(1);   /* back at once */
                else if (play_state == 0) ui_req = 4;   /* load: slow, so in the task */
                return 0;
            }
            if (key == MODULE_KEY_UNPRESS_SET) return 0;
        }
        if (play_state == 2) play_restore(0);  /* back to Canon, then the key */
        else if (play_state == 1) play_abort = 1;
        return 1;
    }
    if (!ui_open) {
        /* SET on the plain shooting screen: nothing else uses it there */
        if (key == MODULE_KEY_PRESS_SET && !gui_menu_shown() && !lv &&
            display_idle() && !is_movie_mode() && soup_ui_count() > 0) {
            ui_req = 1;
            return 0;
        }
        return 1;
    }

    /* DEVELOPED page: any key goes back to the film list */
    if (ui_show_done && key != MODULE_KEY_PRESS_HALFSHUTTER &&
        key != MODULE_KEY_UNPRESS_HALFSHUTTER) {
        if (key == MODULE_KEY_TRASH || key == MODULE_KEY_MENU) { ui_req = 3; return 0; }
        ui_show_done = 0;
        ui_dirty = 1;
        return 0;
    }
    /* DEVELOPING page: the wheel opens the film list; SET does nothing */
    {
        soup_progress p;
        soup_ui_get_progress(&p);
        if (p.busy && !ui_view_stock) {
            switch (key) {
            case MODULE_KEY_WHEEL_LEFT: case MODULE_KEY_WHEEL_RIGHT:
            case MODULE_KEY_WHEEL_UP:   case MODULE_KEY_WHEEL_DOWN:
            case MODULE_KEY_PRESS_LEFT: case MODULE_KEY_PRESS_RIGHT:
                ui_view_stock = 1; ui_dirty = 1; return 0;
            case MODULE_KEY_TRASH: case MODULE_KEY_MENU:
                ui_req = 3; return 0;
            case MODULE_KEY_PRESS_HALFSHUTTER:
                ui_req = 3; return 1;
            default:
                return 0;
            }
        }
    }

    switch (key) {
    case MODULE_KEY_WHEEL_LEFT:
    case MODULE_KEY_WHEEL_UP:
    case MODULE_KEY_PRESS_LEFT:
        se_ui_key_press(&ui, SE_KEY_LEFT);
        ui_dirty = 1;
        return 0;
    case MODULE_KEY_WHEEL_RIGHT:
    case MODULE_KEY_WHEEL_DOWN:
    case MODULE_KEY_PRESS_RIGHT:
        se_ui_key_press(&ui, SE_KEY_RIGHT);
        ui_dirty = 1;
        return 0;
    case MODULE_KEY_PRESS_SET:
        ui_req = 2;
        return 0;
    case MODULE_KEY_INFO:
        ui_req = 5;                /* favourite: done on the UI task */
        return 0;
    case MODULE_KEY_TRASH:
    case MODULE_KEY_MENU:
        ui_req = 3;
        return 0;
    case MODULE_KEY_PRESS_HALFSHUTTER:
        ui_req = 3;
        return 1;                  /* and let the camera focus */
    default:
        return 0;                  /* nothing else reaches ML's menu */
    }
}

void soup_ui_start(void)
{
    task_create("soup_ui", 0x1c, 0x10000, soup_ui_task, 0);   /* 64 KB: the playback viewer decodes here */
}
