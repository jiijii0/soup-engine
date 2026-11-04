/*
 * ui.c - the screens behind INFO: the cans, the counter, the two toggles, and
 * Home with its pages.
 *
 * screen.c decides what the buttons mean; this draws. Everything here runs
 * on one task, soup_ui, which is what keeps it simple: the key callback only
 * leaves notes for the task, so a half-press goes straight through to Canon
 * and nothing can draw while the screen is being put back.
 *
 * The art comes from doc/menu-sample/index.html, rendered once on a desktop
 * by tools/mkui.js into ML/DATA/SOUPUI.BIN: a 256-colour palette, a
 * background, every can at each of the five places it can stand, the panels,
 * and the fonts as alpha glyphs. It is read when the screen opens and freed
 * when it closes. It has to be freed: it comes from the memory Canon wants
 * back before some of its settings change, the same memory a develop uses.
 *
 * Frames are composed in fb, here in cached RAM, and copied to the screen a
 * row at a time. Not in Magic Lantern's "idle" buffer, the obvious place,
 * because that is Canon's own back buffer and Canon draws in it.
 *
 * Canon is kept off the screen with canon_gui_disable_front_buffer, which is
 * what Magic Lantern's own menu does. Magic Lantern's flexinfo needs telling
 * too: it prints the lens, the date and the time over the photo screen every
 * few moments and only stops for its own menu, so while this screen is up
 * everything of theirs is pointed at the back buffer with bmp_draw_to_idle,
 * where nobody sees it. ui_flip goes to the front buffer by name, so it is
 * not affected. The first camera run showed why that matters: painting over
 * flexinfo instead is a screen that flashes for ever.
 *
 * The palette is loaded while the
 * screen is open and put back when it closes. The original entries are in
 * LCD_Palette, so putting them back is exact. Canon can load its palette
 * again at any moment and nobody says when, which is why Magic Lantern
 * refreshes its own once a second and why this does too. Entries 0, 1, 2,
 * 3, 4, 20 and 255 are never touched: the transparent ones, the zebra grid,
 * ClearScreen, and white and black, so that anything Magic Lantern prints
 * over this stays readable.
 *
 * None of this has run on a camera yet, so se_ui_close logs what the first
 * run has to answer: how long a frame takes, and whether Canon reloaded the
 * palette behind our back.
 */
#include "soupeng.h"

#define UI_W      720
#define UI_H      480
#define UI_PITCH  960           /* bytes per row of the bitmap layer */
#define UI_PACK   "ML/DATA/SOUPUI.BIN"
#define UI_CHUNK  (64 * 1024)   /* card to DMA buffer to cached RAM */
#define UI_MAGIC  0x31495553    /* "SUI1" */
#define UI_FLOOR  244           /* the line every can stands on */
#define UI_KEEP   0xffffffffu   /* a palette entry that stays Canon's */

/* Sprite ids: tools/mkui.js's SP. A place in the table is an id. */
enum {
    SP_HINT_LOAD, SP_HINT_CLOSE, SP_AUTO_OFF, SP_AUTO_ON,
    SP_Q_HALF, SP_Q_FULL, SP_Q_QUARTER,
    SP_TAG0, SP_TAG1, SP_TAG2,
    SP_PILL = 10, SP_PILLHL = 13, SP_PEND = 16, SP_PBUSY = 19, SP_PFULL = 22,
    SP_RING = 25, SP_STRIP = 38, SP_ICON = 39, SP_ROWHL = 45, SP_LISTHL = 48,
    SP_INCARD = 51, SP_HINT_HOME = 52, SP_HINT_DARK = 53, SP_HINT_PRINTS = 54,
    SP_HINT_LENS = 55, SP_HINT_SYS = 56
};
enum { F_NAME, F_18, F_15, F_12, F_11, F_10, F_BODY, F_TITLE, F_LIST, F_25, F_COUNT };
enum { R_WHITE, R_DIM, R_AMBER, R_BODY, R_DARK };
#define SHELF_NONE 4            /* a film the pack has not heard of */

typedef struct { uint16_t w, h; int16_t ox, oy; uint32_t off; } ui_sprite;
typedef struct { uint8_t w, h; int8_t ox, oy; uint16_t adv, pad; uint32_t off; } ui_glyph;
typedef struct {
    uint32_t hash;              /* FNV-1a of the .fs file's NAME */
    char     brand[12], iso[6], process[8];
    uint8_t  shelf, pad;
} ui_film;

typedef struct {
    uint32_t magic, size, pal, bg, sprites, nsprites, films, nfilms, cans, pal2, map, floor;
    uint32_t fonts[10];
    uint8_t  ramp[5][8];
    char     shelf[4][8];
} ui_head;

static uint8_t         *P;      /* the pack */
static uint8_t         *fb;     /* the frame */
static const ui_head   *H;
static const ui_sprite *SPR;
static const ui_glyph  *FONT[F_COUNT];

/* The carousel's order is the pack's, not the card's: position to film, and
 * each film's record, which is NULL for one dropped on the card later. */
static int             ui_order[SE_MAX_STOCKS];
static const ui_film  *ui_meta[SE_MAX_STOCKS];
static int             ui_can[SE_MAX_STOCKS];
static int             ui_n;

/* While photos are up the palette is the cube, and art goes through this. */
static const uint8_t *ui_map;
static int            ui_page;
static uint8_t       *ui_tbuf, *ui_pbuf;        /* six thumbnails; one preview */

/* what the first run has to report */
static int ui_ms_open, ui_ms_read, ui_full_lo, ui_full_hi, ui_step_lo, ui_step_hi;
static int ui_frames, ui_resets, ui_opacity;

/* ------------------------------------------------------------- the pack */

static int ui_read(void)
{
    uint32_t size, got = 0;
    uint8_t *bounce;
    FILE *f;

    if (FIO_GetFileSize(UI_PACK, &size) != 0 || size < sizeof(ui_head)) return -1;
    P = __mem_malloc(size, MEM_TEMPORARY, __FILE__, __LINE__);
    if (!P) { se_logf("ui: no memory for the pack, %d KB", (int)(size >> 10)); return -1; }

    /* FIO reads by DMA and refuses a cached buffer that size, and cached is
     * what a blit wants: uncached reads are four times slower here. */
    bounce = fio_malloc(UI_CHUNK);
    if (!bounce) { se_logf("ui: no DMA buffer"); return -1; }
    f = FIO_OpenFile(UI_PACK, O_RDONLY | O_SYNC);
    if (!f) { fio_free(bounce); return -1; }
    while (got < size) {
        int want = size - got > UI_CHUNK ? UI_CHUNK : (int)(size - got);
        int n = FIO_ReadFile(f, bounce, (size_t)want);
        if (n <= 0) break;
        memcpy(P + got, bounce, (size_t)n);
        got += (uint32_t)n;
    }
    FIO_CloseFile(f);
    fio_free(bounce);
    if (got != size) { se_logf("ui: short read, %d of %d", (int)got, (int)size); return -1; }

    H = (const ui_head *)P;
    if (H->magic != UI_MAGIC || H->size != size) { se_logf("ui: not a pack this build reads"); return -1; }
    return 0;
}

/* Films in the pack's order, then any the pack does not know. */
static void ui_build_order(void)
{
    const ui_film *F = (const ui_film *)(P + H->films);
    static uint8_t placed[SE_MAX_STOCKS];
    int i, s;

    SPR = (const ui_sprite *)(P + H->sprites);
    for (i = 0; i < F_COUNT; i++) FONT[i] = (const ui_glyph *)(P + H->fonts[i]);
    memset(placed, 0, sizeof placed);

    ui_n = 0;
    for (i = 0; i < (int)H->nfilms; i++)
        for (s = 0; s < se_nstocks; s++)
            if (!placed[s] && se_name_id(se_stocks[s].name) == F[i].hash) {
                placed[s] = 1;
                ui_meta[s] = &F[i];
                ui_can[s] = (int)H->cans + i * 5;
                ui_order[ui_n++] = s;
                break;
            }
    for (s = 0; s < se_nstocks; s++)
        if (!placed[s]) {
            ui_meta[s] = 0;
            ui_can[s] = (int)H->cans + (int)H->nfilms * 5;
            ui_order[ui_n++] = s;
        }
}

int se_ui_stock(int pos) { return ui_order[pos]; }
int se_ui_count(void)    { return ui_n; }

int se_ui_pos(int stock)
{
    int p;
    for (p = 0; p < ui_n; p++) if (ui_order[p] == stock) return p;
    return 0;
}

static int ui_shelf_of(int pos)
{
    const ui_film *m = ui_meta[ui_order[pos]];
    return m ? m->shelf : SHELF_NONE;
}

/* The first film on the next shelf that has any. */
int se_ui_shelf_jump(int pos, int dir)
{
    int cur = ui_shelf_of(pos), k, p;
    for (k = 1; k <= SHELF_NONE + 1; k++) {
        int want = (cur + dir * k + (SHELF_NONE + 1) * k) % (SHELF_NONE + 1);
        for (p = 0; p < ui_n; p++) if (ui_shelf_of(p) == want) return p;
    }
    return pos;
}

/* ---------------------------------------------------------- the palette */

static void ui_pal_write(int i, uint32_t v)
{
    EngDrvOut(LCD_Palette[3 * i], v);
    EngDrvOut(LCD_Palette[3 * i + 0x300], v);
}

/* Once when the screen opens and once a second after that. */
static const uint32_t *ui_pal_now(void)
{
    return (const uint32_t *)(P + (ui_map ? H->pal2 : H->pal));
}

/* System's Screen colour turns the background and the dim text, entries 5 to
 * 68; the cans keep theirs. Entry is 0x00YYUUVV, with signed U and V. */
static uint32_t ui_tint(int i, uint32_t v)
{
    int y = (v >> 16) & 255, u = (int8_t)(v >> 8), w = (int8_t)v;
    if (i < 41 && !(i >= 12 && i <= 18)) return v;       /* of the text, only DIM */
    if (i > 68 || !se_ui_colour) return v;
    if (se_ui_colour == 2) { u = 0; w = 0; }
    else { int t = u; u = -t / 3; w = t; }               /* blue to ember */
    return ((uint32_t)y << 16) | ((uint32_t)(u & 255) << 8) | (uint32_t)(w & 255);
}

static void ui_pal_apply(void)
{
    const uint32_t *pal = ui_pal_now();
    int i;
    for (i = 0; i < 256; i++)
        if (pal[i] != UI_KEEP) ui_pal_write(i, ui_opacity | ui_tint(i, pal[i]));
}

void se_ui_apply_palette(void) { ui_pal_apply(); }

static void ui_pal_restore(void)
{
    const uint32_t *pal = (const uint32_t *)(P + H->pal);
    int i;
    for (i = 0; i < 256; i++)
        if (pal[i] != UI_KEEP) ui_pal_write(i, LCD_Palette[3 * i + 2]);
}

/* Did Canon put its own back? One entry is enough to tell, and a read of 0
 * means this register does not read back, which says nothing either way. */
void se_ui_pal_tick(void)
{
    const uint32_t *pal = ui_pal_now();
    uint32_t want = (uint32_t)ui_opacity | ui_tint(100, pal[100]);
    uint32_t now  = shamem_read(LCD_Palette[3 * 100]);
    if (pal[100] != UI_KEEP && now && now != want) ui_resets++;
    ui_pal_apply();
}

/* ------------------------------------------------------------ the blits */

static void ui_blit(int id, int x, int y)
{
    const ui_sprite *s = &SPR[id];
    const uint8_t *p = P + s->off;
    int w = s->w, h = s->h, sx = 0;

    if (x < 0) { sx = -x; w += x; x = 0; }
    if (x + w > UI_W) w = UI_W - x;
    if (y < 0) { p += (-y) * s->w; h += y; y = 0; }
    if (y + h > UI_H) h = UI_H - y;
    if (w <= 0 || h <= 0) return;
    for (; h > 0; h--, y++, p += s->w) {
        const uint8_t *q = p + sx;
        uint8_t *d = fb + y * UI_W + x;
        int n = w;
        if (ui_map) while (n--) { uint8_t c = *q++; if (c) *d = ui_map[c]; d++; }
        else        while (n--) { uint8_t c = *q++; if (c) *d = c; d++; }
    }
}

/* A sprite placed by the anchor it was cut around. */
static void ui_at(int id, int cx, int cy) { ui_blit(id, cx + SPR[id].ox, cy + SPR[id].oy); }

/* The same, at scale/256 of its size, nearest neighbour: a can between two
 * places, on screen for a frame or two. */
static void ui_scaled_w(int id, int cx, int cy, int scale, int srcw);
static void ui_scaled(int id, int cx, int cy, int scale) { ui_scaled_w(id, cx, cy, scale, 0); }

/* srcw, if set, stops at that many source columns: a can without its leader. */
static void ui_scaled_w(int id, int cx, int cy, int scale, int srcw)
{
    const ui_sprite *s = &SPR[id];
    const uint8_t *p = P + s->off;
    int dw = (s->w * scale) >> 8, dh = (s->h * scale) >> 8;
    int x0 = cx + ((s->ox * scale) >> 8), y0 = cy + ((s->oy * scale) >> 8);
    int step = (256 << 8) / scale, x, y;

    if (srcw) dw = (srcw * scale) >> 8;
    else if (scale == 256) { ui_blit(id, x0, y0); return; }
    for (y = 0; y < dh; y++) {
        int dy = y0 + y, u = 0;
        const uint8_t *row;
        uint8_t *d;
        if (dy < 0 || dy >= UI_H) continue;
        row = p + ((y * step) >> 8) * s->w;
        d = fb + dy * UI_W;
        for (x = 0; x < dw; x++, u += step) {
            int dx = x0 + x;
            uint8_t c = row[u >> 8];
            if (c && dx >= 0 && dx < UI_W) d[dx] = c;
        }
    }
}

static void ui_rect(int x, int y, int w, int h, uint8_t c)
{
    for (; h > 0; h--, y++) memset(fb + y * UI_W + x, c, (size_t)w);
}

/* Left cap, a column as many times as it takes, right cap. */
static void ui_pill(int id, int x, int y, int w)
{
    int cap = SPR[id].w, i;
    ui_blit(id, x, y);
    for (i = x + cap; i < x + w - cap; i++) ui_blit(id + 1, i, y);
    ui_blit(id + 2, x + w - cap, y);
}

/* `base` is the baseline, `sp` extra spacing in 1/16 of a pixel. Returns the
 * x after the text. Levels 1-7 of a glyph are the steps of the colour's
 * ramp, faded into the screen's mid tone: near enough for edge pixels, and
 * it spares every pixel a lookup into what is behind it. */
static int ui_text(int font, int x, int base, int ramp, int sp, const char *s)
{
    const ui_glyph *G = FONT[font];
    const uint8_t *r = H->ramp[ramp];
    int pen = x << 4;
    for (; *s; s++) {
        int c = (uint8_t)*s, gx, gy, i, j;
        const ui_glyph *g;
        if (c < 32 || c > 127) c = '?';
        g = &G[c - 32];
        gx = (pen >> 4) + g->ox;
        gy = base + g->oy;
        if (g->w && gx >= 0 && gx + g->w <= UI_W && gy >= 0 && gy + g->h <= UI_H) {
            const uint8_t *a = P + g->off;
            for (j = 0; j < g->h; j++) {
                uint8_t *d = fb + (gy + j) * UI_W + gx;
                for (i = 0; i < g->w; i++, d++, a++) if (*a) *d = r[*a];
            }
        }
        pen += g->adv + sp;
    }
    return pen >> 4;
}

static int ui_text_w(int font, int sp, const char *s)
{
    const ui_glyph *G = FONT[font];
    int w = 0;
    for (; *s; s++) {
        int c = (uint8_t)*s;
        if (c < 32 || c > 127) c = '?';
        w += G[c - 32].adv + sp;
    }
    return w >> 4;
}

static void ui_center(int font, int y, int ramp, int sp, const char *s)
{
    ui_text(font, (UI_W - ui_text_w(font, sp, s)) / 2, y, ramp, sp, s);
}

/* ------------------------------------------------------------ the words */

/* .fs files say PASTEL, MILKY BLACKS. A name goes to Title Case, and a word
 * with a digit in it stays as it is; a description goes to Sentence case. */
static void ui_title(const char *in, char *out, int cap)
{
    int i, first = 1, digit = 0;
    for (i = 0; in[i] && i < cap - 1; i++) {
        char c = in[i];
        if (first) { int j; digit = 0; for (j = i; in[j] && in[j] != ' '; j++) if (in[j] >= '0' && in[j] <= '9') digit = 1; }
        out[i] = (!first && !digit && c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        first = c == ' ';
    }
    out[i] = 0;
}

static void ui_sentence(const char *in, char *out, int cap)
{
    int i;
    for (i = 0; in[i] && i < cap - 1; i++) {
        char c = in[i];
        out[i] = (i && c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    out[i] = 0;
}

static const char *ui_time_text(void)
{
    return se_size == 1 ? "3 min" : se_size == 2 ? "10 s" : "36 s";
}

/* ------------------------------------------------------------- the rows */

static void ui_paint_bg(int y0, int y1)
{
    const uint8_t *bg = P + H->bg;
    memcpy(fb + y0 * UI_W, bg + y0 * UI_W, (size_t)((y1 - y0) * UI_W));
    if (ui_map) {                       /* the logo is the only art in it */
        int y, x;
        for (y = y0; y < y1 && y < 42; y++) for (x = 0; x < 140; x++) {
            uint8_t *d = fb + y * UI_W + x;
            *d = ui_map[*d];
        }
    }
}

/* The shelf the cans stand on, rows 244 to 287: only the film screen has one. */
static void ui_paint_floor(int y1)
{
    memcpy(fb + 244 * UI_W, P + H->floor, (size_t)((y1 - 244) * UI_W));
}

static const char *ui_sec_name(void)
{
    static const char *n[] = { "FILM", "HOME", "DARKROOM", "PRINTS", "PRINTS", "LENS", "SYSTEM" };
    return n[ui_page];
}

/* What the header last showed, so a repaint happens only when it changes. */
static int ui_seen_pending = -1, ui_seen_pct = -1, ui_seen_min = -1;

static void ui_paint_header(void)
{
    struct tm now;
    char clock[8], num[8], pct[8];
    int q = se_pending(), p = se_busy ? se_pct : -1;
    int x, w, id, step;
    const char *name = se_stocks[se_stock_index].name;

    LoadCalendarFromRTC(&now);
    clock[0] = (char)('0' + now.tm_hour / 10); clock[1] = (char)('0' + now.tm_hour % 10);
    clock[2] = ':';
    clock[3] = (char)('0' + now.tm_min / 10);  clock[4] = (char)('0' + now.tm_min % 10);
    clock[5] = 0;
    ui_seen_pending = q; ui_seen_pct = p; ui_seen_min = now.tm_min;

    x = 704 - ui_text_w(F_15, 0, clock);
    ui_text(F_15, x, 27, R_WHITE, 0, clock);

    snprintf(num, sizeof num, "%d", q);
    pct[0] = 0;
    if (p >= 0) { int n = snprintf(pct, sizeof pct, "%d", p); pct[n] = '%'; pct[n + 1] = 0; }
    w = 6 + 19 + 7 + ui_text_w(F_18, 0, num) + 7 + ui_text_w(F_12, 19, "PENDING")
      + (p >= 0 ? 7 + ui_text_w(F_12, 6, pct) : 0) + 11;
    x -= 10 + w;
    id = q >= SE_QUEUE ? SP_PFULL : q ? SP_PBUSY : SP_PEND;
    ui_pill(id, x, 7, w);
    step = p >= 0 ? p * 12 / 100 : 0;
    ui_blit(SP_RING + step, x + 6, 11);
    {
        int tx = ui_text(F_18, x + 32, 27, q ? R_AMBER : R_DIM, 0, num) + 7;
        tx = ui_text(F_12, tx, 25, q ? R_WHITE : R_DIM, 19, "PENDING");
        if (p >= 0) ui_text(F_12, tx + 7, 25, R_AMBER, 6, pct);
    }

    if (se_roll_on) {
        char c[16];
        snprintf(c, sizeof c, "\x7f %d", se_roll_shot);
        x -= ui_text_w(F_12, 12, c) + 6;
        ui_text(F_12, x + 3, 25, R_DIM, 12, c);
        x += 3;
    }
    x -= 10 + ui_text_w(F_12, 12, name);
    ui_text(F_12, x, 25, R_WHITE, 12, name);
    ui_rect(x - 12, 19, 6, 4, H->ramp[R_AMBER][7]);        /* a dot, near enough */
    ui_rect(x - 11, 18, 4, 6, H->ramp[R_AMBER][7]);

    ui_rect(83, 13, 1, 16, H->ramp[R_DIM][2]);
    ui_text(F_12, 94, 25, R_DIM, 20, ui_sec_name());
}

static void ui_paint_tabs(int film_shelf)
{
    int i, w = 0, x;
    for (i = 0; i < 4; i++) w += ui_text_w(F_12, 38, H->shelf[i]) + (i ? 30 : 0);
    x = (UI_W - w) / 2;
    for (i = 0; i < 4; i++) {
        int tw = ui_text_w(F_12, 38, H->shelf[i]);
        ui_text(F_12, x, 50, i == film_shelf ? R_AMBER : R_DIM, 38, H->shelf[i]);
        if (i == film_shelf) ui_rect(x + tw / 2 - 9, 57, 18, 2, H->ramp[R_AMBER][7]);
        x += tw + 30;
    }
}

static void ui_paint_dots(int pos)
{
    int p, w = 0, x;
    uint8_t c = H->ramp[R_DIM][5], soft = H->ramp[R_DIM][2];
    for (p = 0; p < ui_n; p++)
        w += (p == pos ? 17 : 3) + (p ? 7 : 0) + (p && ui_shelf_of(p) != ui_shelf_of(p - 1) ? 9 : 0);
    x = (UI_W - w) / 2;
    for (p = 0; p < ui_n; p++) {
        if (p) x += 7 + (ui_shelf_of(p) != ui_shelf_of(p - 1) ? 9 : 0);
        if (p == pos) { ui_rect(x, 262, 17, 3, H->ramp[R_AMBER][7]); x += 17; }
        else {
            ui_rect(x, 262, 3, 3, soft);
            ui_rect(x + 1, 262, 1, 3, c);
            ui_rect(x, 263, 3, 1, c);
            x += 3;
        }
    }
}

/* The places a can stands: distance from the middle, its scale, and the
 * sprite for each side. Q8 from here on: 256 is one place along. */
static const int ui_slot_x[4] = { 0, 172, 287, 372 };
static const int ui_slot_s[4] = { 256, 195, 143, 108 };

static void ui_paint_can(int stock, int dq, int tag)
{
    int a = dq < 0 ? -dq : dq, i = a >> 8, f = a & 255;
    int x, s, near, id, sc;
    if (i >= 3 || a > (2 << 8) + 128) return;
    x = ui_slot_x[i] + (((ui_slot_x[i + 1] - ui_slot_x[i]) * f) >> 8);
    s = ui_slot_s[i] + (((ui_slot_s[i + 1] - ui_slot_s[i]) * f) >> 8);
    near = (a + 128) >> 8;
    if (near > 2) near = 2;
    id = ui_can[stock] + (near == 0 ? 0 : near == 1 ? (dq > 0 ? 1 : 2) : (dq > 0 ? 3 : 4));
    sc = (s << 8) / ui_slot_s[near];
    x = dq < 0 ? -x : x;
    ui_scaled(id, 360 + x, UI_FLOOR, sc);
    if (tag && !f) ui_at(SP_TAG0 + near, 360 + x, UI_FLOOR);
}

/* `pos` is the film in the middle; `sub` how far the row has slid towards
 * the next one, Q8. Far cans first: the near ones stand in front of them. */
static void ui_paint_cans(int pos, int sub)
{
    int pass, p;
    for (pass = 3; pass >= 0; pass--)
        for (p = 0; p < ui_n; p++) {
            int d = p - pos, dq, a;
            d = ((d % ui_n) + ui_n) % ui_n;
            if (d * 2 > ui_n) d -= ui_n;
            dq = d * 256 - sub;
            a = dq < 0 ? -dq : dq;
            if ((a + 128) >> 8 == pass) ui_paint_can(ui_order[p], dq, ui_order[p] == se_stock_index);
        }
}

static int ui_pillbox(int id, int x, const char *label, const char *value, int r)
{
    int lw = ui_text_w(F_10, 18, label), vw = ui_text_w(F_15, 0, value);
    int w = 10 + lw + 6 + vw + 10;
    if (id >= 0) {
        ui_pill(id, x, 360, w);
        ui_text(F_10, x + 10, 375, r == R_AMBER ? R_AMBER : R_DIM, 18, label);
        ui_text(F_15, x + 10 + lw + 6, 378, r, 0, value);
    }
    return w;
}

static void ui_paint_info(int pos)
{
    int stock = ui_order[pos], x, w;
    const se_stock *st = &se_stocks[stock];
    const ui_film *m = ui_meta[stock];
    const char *iso = m ? m->iso : "-", *proc = m ? m->process : "-";
    char text[64];

    if (m) ui_center(F_11, 284, R_AMBER, 42, m->brand);
    ui_title(st->name, text, sizeof text);
    ui_center(F_NAME, 324, R_WHITE, 0, text);
    ui_sentence(st->desc, text, sizeof text);
    ui_center(F_BODY, 348, R_BODY, 0, text);

    w = ui_pillbox(-1, 0, "ISO", iso, R_WHITE) + 7 + ui_pillbox(-1, 0, "PROCESS", proc, R_WHITE)
      + 7 + ui_pillbox(-1, 0, "PER FRAME", ui_time_text(), R_AMBER);
    x = (UI_W - w) / 2;
    x += ui_pillbox(SP_PILL, x, "ISO", iso, R_WHITE) + 7;
    x += ui_pillbox(SP_PILL, x, "PROCESS", proc, R_WHITE) + 7;
    ui_pillbox(SP_PILLHL, x, "PER FRAME", ui_time_text(), R_AMBER);
}

static void ui_paint_bar(int pos)
{
    int q = se_size == 1 ? SP_Q_FULL : se_size == 2 ? SP_Q_QUARTER : SP_Q_HALF;
    int hint = ui_order[pos] == se_stock_index ? SP_HINT_CLOSE : SP_HINT_LOAD;
    ui_blit(hint, SPR[hint].ox, SPR[hint].oy);
    ui_blit(se_auto ? SP_AUTO_ON : SP_AUTO_OFF, SPR[SP_AUTO_ON].ox, SPR[SP_AUTO_ON].oy);
    ui_blit(q, SPR[q].ox, SPR[q].oy);
}

/* A row at a time: the visible 720 of each 960. */
static void ui_flip(int y0, int y1)
{
    uint8_t *v = bmp_vram_real();
    int y;
    if (!v) return;
    for (y = y0; y < y1; y++) memcpy(v + y * UI_PITCH, fb + y * UI_W, UI_W);
}

/* A message over the bar, for a moment. */
static const char *ui_toast_text;
static int         ui_toast_until;
void se_ui_toast(const char *s) { ui_toast_text = s; ui_toast_until = get_ms_clock() + 1500; }

static void ui_paint_toast(void)
{
    int w;
    if (!ui_toast_text || get_ms_clock() > ui_toast_until) { ui_toast_text = 0; return; }
    w = ui_text_w(F_BODY, 0, ui_toast_text) + 32;
    ui_pill(SP_PEND, (UI_W - w) / 2, 384, w);
    ui_text(F_BODY, (UI_W - w) / 2 + 16, 402, R_WHITE, 0, ui_toast_text);
}
int se_ui_toast_live(void) { return ui_toast_text != 0; }

/* ---------------------------------------------------------- whole frames */

static void ui_timed(int t0, int *lo, int *hi)
{
    int ms = get_ms_clock() - t0;
    if (!*hi || ms < *lo) *lo = ms;
    if (ms > *hi) *hi = ms;
    ui_frames++;
}

void se_ui_paint(int pos)
{
    int t0 = get_ms_clock();
    ui_page = PG_FILM;
    ui_paint_bg(0, UI_H);
    ui_paint_floor(288);
    ui_paint_header();
    ui_paint_tabs(ui_shelf_of(pos));
    ui_paint_cans(pos, 0);
    ui_paint_dots(pos);
    ui_paint_info(pos);
    ui_paint_bar(pos);
    ui_paint_toast();
    ui_flip(0, UI_H);
    ui_timed(t0, &ui_full_lo, &ui_full_hi);
}

/* The row part way from pos towards dir: only the band it stands in. The
 * dots, the name and the rest change on the frame it arrives. */
void se_ui_slide(int pos, int dir, int sub)
{
    int t0 = get_ms_clock();
    ui_paint_bg(62, 260);
    ui_paint_floor(260);
    ui_paint_cans(pos, dir * sub);
    ui_flip(62, 260);
    ui_timed(t0, &ui_step_lo, &ui_step_hi);
}

/* Called every 20 ms, and the clock is a question for the RTC: once a second. */
int se_ui_header_stale(void)
{
    static int last_rtc;
    int p = se_busy ? se_pct : -1, t = get_ms_clock();
    if (se_pending() != ui_seen_pending || p != ui_seen_pct) return 1;
    if (t - last_rtc > 1000) {
        struct tm now;
        last_rtc = t;
        LoadCalendarFromRTC(&now);
        return now.tm_min != ui_seen_min;
    }
    return 0;
}

void se_ui_paint_header(void)
{
    ui_paint_bg(0, 40);
    ui_paint_header();
    ui_flip(0, 40);
}

/* Something drew on the layer, ML's info line most likely: put ours back. */
void se_ui_reflip(void) { ui_flip(0, UI_H); }

/*
 * "Loaded": the other cans go, this one steps left, the strip runs out of it
 * to the edge of the screen. Anything that sets *stop cuts it short.
 */
void se_ui_loaded(volatile int *stop)
{
    static const int ease[8] = { 92, 190, 270, 330, 375, 405, 422, 432 };
    int stock = se_stock_index, t0 = get_ms_clock(), i, x;
    char name[40], sub[64];

    ui_title(se_stocks[stock].name, name, sizeof name);
    snprintf(sub, sizeof sub, "%s \x7f %s \x7f Auto %s", name,
             se_size == 1 ? "Full" : se_size == 2 ? "Quarter" : "Half", se_auto ? "on" : "off");

    for (i = 0; i < 8 && !*stop; i++) {
        int cx = 360 - (i >= 3 ? 120 : 120 * (i + 1) / 4);
        ui_paint_bg(0, UI_H);
        ui_paint_floor(288);
        ui_paint_header();
        ui_scaled(ui_can[stock], cx, UI_FLOOR, 256);
        /* over the can's short leader, which is part of its sprite */
        for (x = 0; x < ease[i] + 17; x += 17) ui_blit(SP_STRIP, cx + 52 + x, 112);
        if (i >= 3) {
            ui_center(F_11, 312, R_AMBER, 42, "IN CAMERA");
            ui_center(F_NAME, 353, R_WHITE, 0, "Loaded");
            ui_center(F_BODY, 375, R_BODY, 0, sub);
        }
        ui_flip(0, UI_H);
        msleep(40);
    }
    while (!*stop && get_ms_clock() - t0 < 1400) msleep(20);
}

/* ---------------------------------------------------------------- doors */

static void ui_release(void)
{
    if (ui_tbuf) { free(ui_tbuf); ui_tbuf = 0; }
    if (ui_pbuf) { free(ui_pbuf); ui_pbuf = 0; }
    ui_map = 0; ui_page = PG_FILM;
    if (fb) { free(fb); fb = 0; }
    if (P)  { free(P);  P = 0; }
    H = 0;
}

/* ---------------------------------------------------------- behind Home */

int se_home_sel, se_dark_sel, se_print_sel, se_sys_sel, se_ui_sheet;

/* What is drawn for a row of the Darkroom: queue first, then the raws. */
static int ui_in_queue(int no)
{
    int i;
    for (i = se_q_head; i < se_q_tail; i++)
        if (se_file_no(se_q[i % SE_QUEUE].path) == no) return 1;
    return 0;
}

int se_ui_dark_raw(int row)
{
    int i, n = se_q_count();
    if (row < n) return -1;
    for (i = 0; i < se_nraws; i++) {
        if (ui_in_queue(se_raws[i])) continue;
        if (row == n) return se_raws[i];
        n++;
    }
    return -1;
}

int se_ui_dark_count(void)
{
    int i, n = se_q_count();
    for (i = 0; i < se_nraws; i++) if (!ui_in_queue(se_raws[i])) n++;
    return n;
}

static void ui_right(int font, int x, int base, int ramp, const char *s)
{
    ui_text(font, x - ui_text_w(font, 0, s), base, ramp, 0, s);
}

static void ui_hint(int id) { ui_blit(id, SPR[id].ox, SPR[id].oy); }

static void ui_paint_home(void)
{
    static const char *title[SE_HOME_ROWS] = { "Film", "Darkroom", "Prints", "Roll", "Lens", "System" };
    char sub[64], name[40];
    int i, stock = se_stock_index;
    const ui_film *m = ui_meta[stock];

    ui_blit(SP_INCARD, SPR[SP_INCARD].ox, SPR[SP_INCARD].oy);
    ui_text(F_11, 126 - ui_text_w(F_11, 42, "IN CAMERA") / 2, 70, R_DIM, 42, "IN CAMERA");
    ui_scaled_w(ui_can[stock], 126, 252, 230, 118);
    if (se_roll_on) {
        snprintf(sub, sizeof sub, "ROLL %03d", se_roll_no);
        ui_text(F_11, 126 - ui_text_w(F_11, 42, sub) / 2, 284, R_AMBER, 42, sub);
        snprintf(sub, sizeof sub, "%d %s \x7f %s", se_roll_shot, se_roll_shot == 1 ? "shot" : "shots", se_size == 1 ? "Full" : se_size == 2 ? "Quarter" : "Half");
    } else {
        if (m) ui_text(F_11, 126 - ui_text_w(F_11, 42, m->brand) / 2, 284, R_AMBER, 42, m->brand);
        snprintf(sub, sizeof sub, "%s \x7f Auto %s", se_size == 1 ? "Full" : se_size == 2 ? "Quarter" : "Half", se_auto ? "on" : "off");
    }
    ui_title(se_stocks[stock].name, name, sizeof name);
    ui_text(F_25, 126 - ui_text_w(F_25, 0, name) / 2, 312, R_WHITE, 0, name);
    ui_text(F_BODY, 126 - ui_text_w(F_BODY, 0, sub) / 2, 336, R_BODY, 0, sub);

    for (i = 0; i < SE_HOME_ROWS; i++) {
        int y = 50 + i * 60;
        if (i == se_home_sel) ui_pill(SP_ROWHL, 248, y, 456);
        ui_blit(SP_ICON + i, 262, y + 17);
        ui_text(F_TITLE, 299, y + 28, R_WHITE, 0, title[i]);
        switch (i) {
        case 0: snprintf(sub, sizeof sub, "Change the film in the camera"); break;
        case 1:
            if (se_pending() || se_ui_dark_count()) snprintf(sub, sizeof sub, "%d pending \x7f %d not developed", se_pending(), se_ui_dark_count() - se_q_count());
            else snprintf(sub, sizeof sub, "Every frame is developed");
            break;
        case 2: snprintf(sub, sizeof sub, "%d on the card", se_nprints); break;
        case 3:
            if (se_roll_on) snprintf(sub, sizeof sub, "Roll %03d \x7f %d %s", se_roll_no, se_roll_shot, se_roll_shot == 1 ? "shot" : "shots");
            else snprintf(sub, sizeof sub, "Off \x7f SET counts your shots");
            break;
        case 4: snprintf(sub, sizeof sub, "%s", lens_info.name[0] ? lens_info.name : "No lens data"); break;
        default: snprintf(sub, sizeof sub, "%s \x7f closes after %d s", se_ui_colour == 1 ? "Ember" : se_ui_colour == 2 ? "Mono" : "Night", se_ui_idle); break;
        }
        ui_text(F_BODY, 299, y + 46, R_BODY, 0, sub);
        ui_text(F_12, 684, y + 34, R_DIM, 0, ">");
    }
    ui_hint(SP_HINT_HOME);
}

static void ui_paint_dark(void)
{
    int total = se_ui_dark_count(), nq = se_q_count(), first, row, y = 50, last = -1;
    char a[16];

    if (se_dark_sel >= total) se_dark_sel = total ? total - 1 : 0;
    if (!total) {
        ui_center(F_LIST, 200, R_DIM, 0, "Every frame on the card is developed.");
        ui_center(F_LIST, 222, R_DIM, 0, "Turn Auto off and shoot to fill this list.");
        ui_hint(SP_HINT_DARK);
        return;
    }
    first = se_dark_sel - 3;
    if (first > total - 8) first = total - 8;
    if (first < 0) first = 0;
    for (row = first; row < total && row < first + 8; row++) {
        int q = row < nq, stock, size, no;
        const se_job *j = q ? &se_q[(se_q_head + row) % SE_QUEUE] : 0;
        if (q != last) {
            ui_text(F_11, 32, y + 16, R_DIM, 42, q ? "DEVELOPING" : "NOT DEVELOPED");
            y += 23; last = q;
        }
        no = q ? se_file_no(j->path) : se_ui_dark_raw(row);
        stock = q ? j->stock : se_stock_index;       /* a raw gets the film in the camera */
        size = q ? j->quality : se_size;
        if (row == se_dark_sel) ui_pill(SP_LISTHL, 20, y, 680);
        snprintf(a, sizeof a, "IMG_%04d", no);
        ui_text(F_LIST, 32, y + 25, R_WHITE, 0, a);
        ui_text(F_15, 152, y + 25, R_BODY, 12, se_stocks[stock].name);
        ui_text(F_LIST, 302, y + 25, R_DIM, 0, se_size_name(size));
        if (q && row == 0 && se_busy) {
            char pct[8]; int n = snprintf(pct, sizeof pct, "%d", se_pct);
            pct[n] = '%'; pct[n + 1] = 0;
            ui_rect(372, y + 17, 120, 6, H->ramp[R_DIM][2]);
            ui_rect(372, y + 17, 120 * se_pct / 100, 6, H->ramp[R_AMBER][7]);
            ui_right(F_LIST, 688, y + 25, R_DIM, pct);
        } else if (q) ui_right(F_LIST, 688, y + 25, R_DIM, j->ready ? "waiting" : "writing");
        y += 40;
    }
    ui_hint(SP_HINT_DARK);
}

static void ui_kv(int y, const char *k, const char *v, int on)
{
    if (on) ui_pill(SP_LISTHL, 20, y, 680);
    ui_text(F_LIST, 32, y + 25, R_WHITE, 0, k);
    ui_right(F_15, 688, y + 26, R_AMBER, v);
}

static void ui_paint_lens(void)
{
    char v[16];
    ui_text(F_11, 32, 66, R_DIM, 42, "FROM THE LENS");
    ui_kv(73, "Lens", lens_info.name[0] ? lens_info.name : "No lens data", 0);
    snprintf(v, sizeof v, "%d mm", (int)lens_info.focal_len);
    ui_kv(113, "Focal length", v, 0);
    ui_kv(153, "Aperture", lens_format_aperture(lens_info.raw_aperture), 0);
    ui_kv(193, "Shutter", lens_format_shutter(lens_info.raw_shutter), 0);
    ui_kv(233, "ISO", lens_format_iso(lens_info.raw_iso), 0);
    ui_hint(SP_HINT_LENS);
}

static void ui_paint_system(void)
{
    char v[24];
    ui_text(F_11, 32, 66, R_DIM, 42, "SYSTEM");
    ui_kv(73, "Screen colour", se_ui_colour == 1 ? "Ember" : se_ui_colour == 2 ? "Mono" : "Night", se_sys_sel == 0);
    snprintf(v, sizeof v, "%d s", se_ui_idle);
    ui_kv(113, "Closes after", v, se_sys_sel == 1);
    snprintf(v, sizeof v, "%d found", se_nstocks);
    ui_kv(153, "Rescan films", v, se_sys_sel == 2);
    ui_kv(193, "Canon menu", "Open", se_sys_sel == 3);
    ui_hint(SP_HINT_SYS);
}

/* The six on show, read when the page turns and when a preview arrives. */
static int  ui_tile_page = -1, ui_tile_gen = -1;
static int  ui_tile_ok[6], ui_tile_size[6];
static char ui_tile_film[6][24];
static int  ui_view_no = -1, ui_view_ok, ui_view_size, ui_view_gen = -1;
static char ui_view_film[24];

static void ui_load_tiles(void)
{
    int pg = se_print_sel / 6, i, want = 0;
    ui_tile_page = pg;
    ui_tile_gen = se_preview_gen;
    for (i = 0; i < 6; i++) {
        int k = pg * 6 + i;
        ui_tile_ok[i] = 0;
        if (k >= se_nprints) continue;
        if (se_pv_info(se_prints[k], ui_tile_film[i], sizeof ui_tile_film[i], &ui_tile_size[i]) == 0
            && se_pv_thumb(se_prints[k], ui_tbuf + i * SE_TH_W * SE_TH_H) == 0) ui_tile_ok[i] = 1;
        else if (!want) want = se_prints[k];
    }
    if (want && !se_preview_want) se_preview_want = want;
}

static void ui_load_view(void)
{
    int no = se_nprints ? se_prints[se_print_sel] : -1;
    ui_view_no = no;
    ui_view_gen = se_preview_gen;
    ui_view_ok = 0;
    if (no < 0) return;
    if (se_pv_info(no, ui_view_film, sizeof ui_view_film, &ui_view_size) == 0 && se_pv_full(no, ui_pbuf) == 0)
        ui_view_ok = 1;
    else if (!se_preview_want) se_preview_want = no;
}

/* Called every 20 ms while photos are up. 1: there is something new to show. */
int se_ui_prints_tick(int page)
{
    if (page == PG_PRINTS && (se_print_sel / 6 != ui_tile_page || se_preview_gen != ui_tile_gen)) { ui_load_tiles(); return 1; }
    if (page == PG_VIEW && (ui_view_no != (se_nprints ? se_prints[se_print_sel] : -1) || (!ui_view_ok && se_preview_gen != ui_view_gen))) { ui_load_view(); return 1; }
    return 0;
}

static void ui_paint_prints(void)
{
    int i, pg = se_print_sel / 6;
    char a[16];
    if (!se_nprints) { ui_center(F_LIST, 210, R_DIM, 0, "No prints on the card yet."); ui_hint(SP_HINT_PRINTS); return; }
    for (i = 0; i < 6 && pg * 6 + i < se_nprints; i++) {
        int x = 24 + (i % 3) * 228, y = 46 + (i / 3) * 169, row;
        int sel = pg * 6 + i == se_print_sel;
        if (sel) ui_rect(x - 3, y - 3, SE_TH_W + 6, SE_TH_H + 6, H->ramp[R_AMBER][7]);
        if (ui_tile_ok[i])
            for (row = 0; row < SE_TH_H; row++)
                memcpy(fb + (y + row) * UI_W + x, ui_tbuf + (size_t)i * SE_TH_W * SE_TH_H + row * SE_TH_W, SE_TH_W);
        else {
            ui_rect(x, y, SE_TH_W, SE_TH_H, COLOR_BLACK);
            ui_text(F_12, x + 80, y + 76, R_DIM, 0, "Developing");
        }
        snprintf(a, sizeof a, "FLM_%04d", se_prints[pg * 6 + i]);
        ui_text(F_12, x, y + SE_TH_H + 15, sel ? R_WHITE : R_DIM, 6, a);
        if (ui_tile_ok[i]) ui_right(F_12, x + SE_TH_W, y + SE_TH_H + 15, sel ? R_WHITE : R_DIM, ui_tile_film[i]);
    }
    ui_hint(SP_HINT_PRINTS);
}

static void ui_paint_view(void)
{
    char a[32];
    ui_rect(0, 0, UI_W, UI_H, COLOR_BLACK);
    if (ui_view_ok) {
        int row;
        for (row = 0; row < SE_PV_H; row++) memcpy(fb + row * UI_W + 36, ui_pbuf + row * SE_PV_W, SE_PV_W);
    } else ui_center(F_LIST, 216, R_DIM, 0, se_busy ? "Opening print, after this frame develops" : "Opening print");
    snprintf(a, sizeof a, "FLM_%04d.JPG", ui_view_no);
    ui_text(F_15, 14, 450, R_WHITE, 0, a);
    if (ui_view_ok) {
        snprintf(a, sizeof a, "%s \x7f %s", ui_view_film[0] ? ui_view_film : "-", se_size_name(ui_view_size));
        ui_center(F_15, 450, R_WHITE, 0, a);
    }
    snprintf(a, sizeof a, "%d / %d", se_print_sel + 1, se_nprints);
    ui_right(F_15, 706, 450, R_WHITE, a);
    snprintf(a, sizeof a, "Q Redevelop in %s", se_stocks[se_stock_index].name);
    ui_text(F_12, 14, 471, R_DIM, 0, "<> Next     ");
    ui_text(F_12, 90, 471, R_DIM, 0, a);
    ui_right(F_12, 706, 471, R_DIM, "TRASH Delete print     MENU Back");
}

static void ui_paint_sheet(void)
{
    char t[32];
    ui_rect(20, 362, 680, 118, H->ramp[R_DIM][1]);
    ui_rect(20, 362, 680, 1, H->ramp[R_DIM][4]);
    snprintf(t, sizeof t, "Delete FLM_%04d.JPG?", ui_view_no);
    ui_text(F_25, 40, 395, R_WHITE, 0, t);
    ui_text(F_BODY, 40, 418, R_BODY, 0, "The raw stays on the card. It can be developed again from the Darkroom.");
    ui_text(F_15, 40, 450, R_AMBER, 0, "SET  Delete print");
    ui_text(F_15, 190, 450, R_WHITE, 0, "MENU  Keep it");
}

void se_ui_enter(int page)
{
    int photos = page == PG_PRINTS || page == PG_VIEW;
    ui_page = page;
    ui_map = (photos && (ui_tbuf || (ui_tbuf = __mem_malloc(6 * SE_TH_W * SE_TH_H, MEM_TEMPORARY, __FILE__, __LINE__)))
                     && (ui_pbuf || (ui_pbuf = __mem_malloc(SE_PV_W * SE_PV_H, MEM_TEMPORARY, __FILE__, __LINE__))))
           ? P + H->map : 0;
    if (photos && !ui_map) se_ui_toast("No memory for prints");
    ui_pal_apply();
    if (page == PG_HOME || page == PG_DARK || page == PG_PRINTS) se_walk_card();
    if (se_nprints && se_print_sel >= se_nprints) se_print_sel = se_nprints - 1;
    ui_tile_page = -1; ui_view_no = -2;
    if (ui_map) se_ui_prints_tick(page);
}

void se_ui_paint_page(int page)
{
    int t0 = get_ms_clock();
    if (page != PG_VIEW) { ui_paint_bg(0, UI_H); ui_paint_header(); }
    switch (page) {
    case PG_HOME:   ui_paint_home();   break;
    case PG_DARK:   ui_paint_dark();   break;
    case PG_PRINTS: if (ui_map) ui_paint_prints(); break;
    case PG_VIEW:   if (ui_map) ui_paint_view(); if (se_ui_sheet) ui_paint_sheet(); break;
    case PG_LENS:   ui_paint_lens();   break;
    case PG_SYSTEM: ui_paint_system(); break;
    }
    ui_paint_toast();
    ui_flip(0, UI_H);
    ui_timed(t0, &ui_full_lo, &ui_full_hi);
}

/* 0, and the screen is ours; or -1, nothing changed, and the caller shows the
 * plain list instead. A film picker that needs 1.9 MB to open must not be
 * the only one. */
int se_ui_open(void)
{
    int t0 = get_ms_clock();
    if (ui_read() != 0) { ui_release(); return -1; }
    ui_ms_read = get_ms_clock() - t0;
    fb = __mem_malloc(UI_W * UI_H, MEM_TEMPORARY, __FILE__, __LINE__);
    if (!fb) { se_logf("ui: no memory for a frame"); ui_release(); return -1; }

    ui_build_order();
    ui_full_lo = ui_full_hi = ui_step_lo = ui_step_hi = ui_frames = ui_resets = 0;
    ui_toast_text = 0;

    /* A palette entry is opacity, Y, U, V. How opaque "opaque" is, is Canon's
     * business; white is opaque, so ask white. */
    ui_opacity = (int)(LCD_Palette[3 * COLOR_WHITE + 2] & 0xff000000u);

    canon_gui_disable_front_buffer();
    bmp_draw_to_idle(1);
    ui_pal_apply();
    ui_ms_open = get_ms_clock() - t0;
    return 0;
}

void se_ui_close(int shutting_down)
{
    if (!P) return;
    ui_pal_restore();
    bmp_draw_to_idle(0);
    canon_gui_enable_front_buffer(0);
    se_logf("ui: opacity %x, pack read in %d ms, open %d ms; full frame %d-%d ms, slide step %d-%d ms, %d frames; Canon reloaded the palette %d times",
            ui_opacity, ui_ms_read, ui_ms_open, ui_full_lo, ui_full_hi, ui_step_lo, ui_step_hi, ui_frames, ui_resets);
    ui_release();
    if (!shutting_down) redraw();
}
