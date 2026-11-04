#include "ui.h"
#include "cr2.h"
#include "se_libc.h"

/* ====================================================== primitives ==== */

void se_ui_px(se_ui_fb *fb, int x, int y, uint8_t c)
{
    uint8_t *p;
    if (x < 0 || y < 0 || x >= SE_UI_W || y >= SE_UI_H) return;
    p = &fb->px[(x >> 1) + y * SE_UI_PITCH];
    /* odd x in the high nibble, matching Magic Lantern's SET_4BIT_PIXEL */
    if (x & 1) *p = (uint8_t)((*p & 0x0F) | (c << 4));
    else       *p = (uint8_t)((*p & 0xF0) | (c & 0x0F));
}

void se_ui_clear(se_ui_fb *fb, uint8_t c)
{
    uint8_t b = (uint8_t)((c & 0x0F) | (c << 4));
    memset(fb->px, b, SE_UI_FB_SIZE);
}

void se_ui_fill(se_ui_fb *fb, int x, int y, int w, int h, uint8_t c)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            se_ui_px(fb, x + i, y + j, c);
}

void se_ui_frame(se_ui_fb *fb, int x, int y, int w, int h, uint8_t c)
{
    se_ui_fill(fb, x,         y,         w, 1, c);
    se_ui_fill(fb, x,         y + h - 1, w, 1, c);
    se_ui_fill(fb, x,         y,         1, h, c);
    se_ui_fill(fb, x + w - 1, y,         1, h, c);
}

static int glyph_index(unsigned char ch)
{
    if (ch >= 0x01 && ch <= 0x04) return (0x5F - 0x20 + 1) + (ch - 1);
    if (ch >= 'a' && ch <= 'z')   ch = (unsigned char)(ch - 'a' + 'A');
    if (ch < 0x20 || ch > 0x5F)   return 0;   /* space */
    return ch - 0x20;
}

int se_ui_text_w(const char *s, int sx)
{
    int n = 0;
    while (*s++) n++;
    if (n == 0) return 0;
    return n * (SE_FONT_W + 1) * sx - sx;   /* no trailing gap */
}

int se_ui_text(se_ui_fb *fb, int x, int y, const char *s,
               uint8_t c, int sx, int sy)
{
    int x0 = x;
    for (; *s; s++) {
        const uint8_t *g = se_ui_font[glyph_index((unsigned char)*s)];
        int col, row;
        for (col = 0; col < SE_FONT_W; col++) {
            uint8_t bits = g[col];
            for (row = 0; row < SE_FONT_H; row++) {
                if (bits & (1u << row))
                    se_ui_fill(fb, x + col * sx, y + row * sy, sx, sy, c);
            }
        }
        x += (SE_FONT_W + 1) * sx;
    }
    return x - x0;
}

int se_ui_text_c(se_ui_fb *fb, int cx, int y, const char *s,
                 uint8_t c, int sx, int sy)
{
    return se_ui_text(fb, cx - se_ui_text_w(s, sx) / 2, y, s, c, sx, sy);
}

/* ------------------------------------------------- tiny string helpers */

static char *put_str(char *d, const char *s)
{
    while (*s) *d++ = *s++;
    *d = 0;
    return d;
}

static char *put_int(char *d, int v)
{
    char tmp[12];
    int  n = 0;
    if (v < 0) { *d++ = '-'; v = -v; }
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *d++ = tmp[--n];
    *d = 0;
    return d;
}

/* ====================================================== settings ====== */

static const char *g_res_name[3]  = { "FULL", "HALF", "QUARTER" };
static const int   g_res_scale[3] = { 1, 2, 4 };
/*
 * The output width each resolution gives, which depends on the source: the
 * sensor frame is 4368 active columns, the JPEG Canon embeds is 2496. The
 * screen said 4368 either way, which was simply wrong half the time.
 */
static const int   g_res_px[2][3] = { { 2496, 1248, 624 },
                                      { 4368, 2184, 1092 } };

/*
 * Short, because this is read on a 2.5-inch panel at arm's length and the
 * word that carries the information is the only word worth having.
 * "FROM PREVIEW JPEG" made the reader work through two words of grammar
 * to get to it.
 */
static const char *g_src_name[2] = { "JPEG", "RAW" };

static const int g_set_max[SE_SET_COUNT] = { 1, 2, 1 };

/*
 * Settings are restored from a file on the card, so they are untrusted
 * input: a truncated or hand-edited config must not be able to index past
 * a name table. Every read of a setting goes through this.
 */
static int setval(const se_ui *u, int which)
{
    int v;
    if (which < 0 || which >= SE_SET_COUNT) return 0;
    v = u->value[which];
    return SE_CLAMP(v, 0, g_set_max[which]);
}

int   se_ui_scale(const se_ui *u)    { return g_res_scale[setval(u, SE_SET_RES)]; }
int   se_ui_grain_on(const se_ui *u) { return setval(u, SE_SET_GRAIN); }
int   se_ui_source(const se_ui *u)   { return setval(u, SE_SET_SOURCE); }

static void setting_label(int i, char *dst)
{
    static const char *n[SE_SET_COUNT] = { "SOURCE", "RESOLUTION", "GRAIN" };
    put_str(dst, (i >= 0 && i < SE_SET_COUNT) ? n[i] : "?");
}

static void setting_value(const se_ui *u, int i, char *dst)
{
    char *d = dst;
    int   v = setval(u, i);
    switch (i) {
    case SE_SET_SOURCE:
        put_str(d, g_src_name[v]);
        break;
    case SE_SET_RES:
        d = put_str(d, g_res_name[v]);
        d = put_str(d, "  ");
        d = put_int(d, g_res_px[se_ui_source(u)][v]);
        put_str(d, " PX");
        break;
    case SE_SET_GRAIN:
        put_str(d, v ? "AS THE STOCK SAYS" : "OFF");
        break;
    default:
        put_str(d, "?");
        break;
    }
}

/* ====================================================== state ========= */

void se_ui_init(se_ui *u)
{
    memset(u, 0, sizeof *u);
    u->screen = SE_SCREEN_STOCK;
    u->stock  = 0;
    u->value[SE_SET_SOURCE] = SE_SOURCE_PREVIEW;
    u->value[SE_SET_RES]    = 1;   /* HALF while we are testing */
    u->value[SE_SET_GRAIN]  = 1;
    u->stage = "";
}

void se_ui_set_stocks(se_ui *u, const se_ui_stock *list, int n)
{
    u->stocks  = list;
    u->nstocks = n < 0 ? 0 : n;
    if (u->stock >= u->nstocks) u->stock = 0;
}

void se_ui_key_press(se_ui *u, se_ui_key k)
{
    int n = u->nstocks;
    if (n < 1) n = 1;

    switch (u->screen) {
    case SE_SCREEN_STOCK:
        if      (k == SE_KEY_LEFT)  u->stock = (u->stock + n - 1) % n;
        else if (k == SE_KEY_RIGHT) u->stock = (u->stock + 1) % n;
        else if (k == SE_KEY_MENU)  u->screen = SE_SCREEN_SETTINGS;
        else if (k == SE_KEY_SET)   u->screen = SE_SCREEN_WORKING;
        break;

    case SE_SCREEN_SETTINGS:
        if      (k == SE_KEY_UP)    u->setting = (u->setting + SE_SET_COUNT - 1) % SE_SET_COUNT;
        else if (k == SE_KEY_DOWN)  u->setting = (u->setting + 1) % SE_SET_COUNT;
        else if (k == SE_KEY_LEFT)  { int *v = &u->value[u->setting];
                                      *v = (*v > 0) ? *v - 1 : g_set_max[u->setting]; }
        else if (k == SE_KEY_RIGHT) { int *v = &u->value[u->setting];
                                      *v = (*v < g_set_max[u->setting]) ? *v + 1 : 0; }
        else if (k == SE_KEY_MENU || k == SE_KEY_SET) u->screen = SE_SCREEN_STOCK;
        break;

    case SE_SCREEN_WORKING:
        if (k == SE_KEY_MENU) u->screen = SE_SCREEN_STOCK;   /* cancel */
        break;

    case SE_SCREEN_DONE:
        u->screen = SE_SCREEN_STOCK;
        break;

    default:
        break;
    }
}

/* ====================================================== drawing ======= */
/*
 * A pixel here is twice as TALL as it is wide on the real screen -- the
 * buffer is 720x240 and Magic Lantern's coordinate space for this body is
 * 720x480, halved vertically. So text takes separate scales and a readable
 * line is sx = 2 * sy, and a shape meant to read as square is twice as
 * wide as it is high.
 *
 * WHAT THIS IS FOR, because the first version forgot: you are holding a
 * camera, the screen is 2.5 inches across, and you want to know which film
 * you are about to shoot. One thing matters -- the name -- and it should
 * be readable at arm's length without looking for it.
 *
 * So the hierarchy is the design. The stock name is set enormous and
 * centred; everything else is small, grey and out of the way. There is one
 * piece of colour per screen, a single band carrying the speed, and it is
 * there to tell the stocks apart at a glance rather than to decorate.
 *
 * The version before this one had cartons with corner wedges, colour
 * chips, barcodes, sprocket rows and a dithered paper texture behind all
 * of it. Every piece of that was legible on a desktop monitor at 3x and
 * none of it was legible on the camera. Detail is not the same as craft.
 */

/* ---------------------------------------------------------- the ground */

static void paper_ground(se_ui_fb *fb)
{
    se_ui_clear(fb, SE_C_PAPER);
}

/* A filled panel with a keyline and its corners knocked off. One pixel at
 * each corner is the difference between a printed card and a filled rect,
 * and it is the only ornament left. */
static void plate(se_ui_fb *fb, int x, int y, int w, int h,
                  uint8_t fill, uint8_t edge, uint8_t ground)
{
    se_ui_fill (fb, x, y, w, h, fill);
    if (edge != fill) se_ui_frame(fb, x, y, w, h, edge);
    se_ui_fill(fb, x,         y,         2, 1, ground);
    se_ui_fill(fb, x + w - 2, y,         2, 1, ground);
    se_ui_fill(fb, x,         y + h - 1, 2, 1, ground);
    se_ui_fill(fb, x + w - 2, y + h - 1, 2, 1, ground);
}

/* Is this a light colour? Sixteen fixed colours, so a list, not a sum. */
static int is_light(uint8_t c)
{
    return c == SE_C_HILITE || c == SE_C_PAPER || c == SE_C_EDGE ||
           c == SE_C_GOLD   || c == SE_C_AMBER || c == SE_C_WHITE;
}
static uint8_t on(uint8_t ground)
{
    return is_light(ground) ? SE_C_INK : SE_C_HILITE;
}

/* ------------------------------------------------------ chrome: bars */

static void header(se_ui_fb *fb, const char *title, const char *right)
{
    se_ui_text(fb, 16, 8, title, SE_C_SLATE, 2, 1);
    if (right)
        se_ui_text(fb, SE_UI_W - 16 - se_ui_text_w(right, 2), 8,
                   right, SE_C_SLATE, 2, 1);
    se_ui_fill(fb, 16, 24, SE_UI_W - 32, 2, SE_C_INK);
}

static void footer(se_ui_fb *fb, const char *hints)
{
    se_ui_fill(fb, 16, 212, SE_UI_W - 32, 1, SE_C_EDGE);
    se_ui_text_c(fb, SE_UI_W / 2, 222, hints, SE_C_MID, 2, 1);
}

/*
 * The one piece of colour: a band with the film speed in it.
 *
 * It is what tells the stocks apart without reading, and it is the only
 * thing on the screen that is not type. One band, one colour, centred
 * under the name.
 */
static void speed_band(se_ui_fb *fb, int y, uint8_t accent, const char *iso)
{
    const int w = 300, x = (SE_UI_W - w) / 2;
    plate(fb, x, y, w, 26, accent, SE_C_INK, SE_C_PAPER);
    se_ui_text_c(fb, SE_UI_W / 2, y + 6, iso, on(accent), 3, 2);
}

/*
 * The name, set as large as it will go.
 *
 * Scale is chosen to fit rather than fixed, so CINE is not dwarfed and
 * KODACHROME is not clipped. The ladder steps down 7, 6, 5, 4 and then
 * truncates -- at scale 4 a character is 20 wide, so 32 of them fit, and
 * no stock name is anywhere near that.
 */
static void big_name(se_ui_fb *fb, int y, const char *s, uint8_t c)
{
    static const int ladder[4] = { 7, 6, 5, 4 };
    int i, sx = 4;
    for (i = 0; i < 4; i++)
        if (se_ui_text_w(s, ladder[i]) <= SE_UI_W - 120) { sx = ladder[i]; break; }
    se_ui_text_c(fb, SE_UI_W / 2, y, s, c, sx, sx > 5 ? 4 : 3);
}

/* --------------------------------------------- per-stock accent colour */
/*
 * One colour each, chosen by name so the card format does not change.
 * An unrecognised stock still gets one, keyed off its own letters, so it
 * is at least consistently itself.
 */
static int name_is(const char *name, const char *want)
{
    int i;
    for (i = 0; want[i]; i++) {
        char a = name[i], b = want[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (a != b) return 0;
    }
    return 1;
}

typedef struct { uint8_t accent; const char *iso; } se_stock_look;

/*
 * The number on the band is the FILM's rated speed -- Velvia is a 50,
 * Portra a 400 -- and nothing else. The engine never touches the camera's
 * ISO, and must not: Canon's exposure settings are properties, and writing
 * a property is forbidden here because they live in NVRAM.
 *
 * So the band says "FILM SPEED 50" rather than "ISO 50". The same number,
 * and no way to read it as something the camera is about to do.
 */
/*
 * name_is() matches a PREFIX, so order and length both matter here.
 * "CLASSIC" would make the two Classics indistinguishable -- hence
 * "CLASSIC CH" and "CLASSIC N".
 * A stock with no entry gets a hashed accent and 200, which is fine for a
 * stock someone drops on the card but not for one that ships.
 */
static const struct { const char *key; se_stock_look lk; } g_looks[] = {
    /* the Fujifilm simulations */
    { "PROVIA",     { SE_C_SLATE,  "FILM SPEED 100" } },
    { "VELVIA",     { SE_C_OLIVE,  "FILM SPEED 50"  } },
    { "ASTIA",      { SE_C_PLUM,   "FILM SPEED 100" } },
    { "CLASSIC CH", { SE_C_MID,    "FILM SPEED 200" } },
    { "CLASSIC N",  { SE_C_TEAL,   "FILM SPEED 400" } },
    { "NOSTALGIC",  { SE_C_RUST,   "FILM SPEED 400" } },
    { "ETERNA",     { SE_C_EDGE,   "FILM SPEED 250" } },
    { "BLEACH",     { SE_C_SHADOW, "FILM SPEED 400" } },
    { "ACROS",      { SE_C_INK,    "FILM SPEED 100" } },
    { "REALA",      { SE_C_HILITE, "FILM SPEED 100" } },
    /* and the few that are not Fuji */
    { "SOUP 800",   { SE_C_RED,    "FILM SPEED 800" } },
    { "GOLD",       { SE_C_GOLD,   "FILM SPEED 200" } },
    { "PORTRA",     { SE_C_AMBER,  "FILM SPEED 400" } },
};
#define SE_NLOOKS ((int)(sizeof g_looks / sizeof g_looks[0]))

static se_stock_look stock_look(const char *name)
{
    static const uint8_t pick[7] = { SE_C_TEAL, SE_C_PLUM, SE_C_AMBER,
                                     SE_C_OLIVE, SE_C_RUST, SE_C_GOLD,
                                     SE_C_RED };
    se_stock_look lk;
    unsigned h = 2166136261u;
    int i;

    if (!name) name = "?";
    for (i = 0; i < SE_NLOOKS; i++)
        if (name_is(name, g_looks[i].key)) return g_looks[i].lk;

    for (i = 0; name[i]; i++) h = (h ^ (unsigned char)name[i]) * 16777619u;
    lk.accent = pick[h % 7u];
    lk.iso    = "FILM SPEED 200";
    return lk;
}


/* ------------------------------------------------------- stock icons */
/*
 * A small piece of pixel art beside each film's name.
 *
 * Twelve by twelve, written as text so it can be read and edited as a
 * picture rather than as hex. '#' is drawn in the name's ink, '+' in the
 * stock's own accent colour, '.' is left alone.
 *
 * Each art pixel is drawn TWICE AS WIDE as it is tall, because the
 * overlay is a 720x240 buffer presented over 480 lines -- so a square in
 * memory is a rectangle on the panel, and only a 2:1 block comes out
 * square to the eye. The font scales already do this (3,2 and 7,4); this
 * is the same correction.
 */
#define SE_ICON_N 12

typedef const char *se_icon[SE_ICON_N];

/* the aperture ring: the neutral one, and the fallback for any stock
 * dropped on the card that this file has never heard of */
static const se_icon ic_iris = {
    "...######...",
    "..#++++++#..",
    ".#++####++#.",
    "#++##..##++#",
    "#++#....#++#",
    "#++#....#++#",
    "#++#....#++#",
    "#++#....#++#",
    "#++##..##++#",
    ".#++####++#.",
    "..#++++++#..",
    "...######..." };

/* VELVIA: mountains. Landscape film, and the most saturated thing here */
static const se_icon ic_peaks = {
    "............",
    "............",
    ".....#......",
    "....#+#.....",
    "...#+++#....",
    "..#+++++#...",
    "..#+++++#.#.",
    ".#+++++++#+#",
    ".#+++++++++#",
    "#+++++++++++",
    "############",
    "............" };

/* ASTIA and PORTRA: a head and shoulders. The films made for people */
static const se_icon ic_bust = {
    "....####....",
    "...#++++#...",
    "..#++++++#..",
    "..#++++++#..",
    "...#++++#...",
    "....####....",
    "....####....",
    "..##++++##..",
    ".#++++++++#.",
    "#++++++++++#",
    "#++++++++++#",
    "#++++++++++#" };

/* CLASSIC CHROME: a camera, because it is the documentary one */
static const se_icon ic_camera = {
    "............",
    "....####....",
    "..##++++##..",
    "############",
    "#++##++##++#",
    "#+#++++++#+#",
    "#+#++##++#+#",
    "#+#++++++#+#",
    "#++##++##++#",
    "############",
    "............",
    "............" };

/* CLASSIC NEG and NOSTALGIC NEG: a strip of negative with sprockets */
static const se_icon ic_strip = {
    "############",
    "#..##..##..#",
    "#..##..##..#",
    "############",
    "#++++++++++#",
    "#++++++++++#",
    "#++++++++++#",
    "#++++++++++#",
    "############",
    "#..##..##..#",
    "#..##..##..#",
    "############" };

/* ETERNA: a clapperboard. Motion picture stock */
static const se_icon ic_clap = {
    "............",
    ".##.##.##.#.",
    ".#.##.##.##.",
    ".##########.",
    ".#++++++++#.",
    ".#++++++++#.",
    ".#++++++++#.",
    ".#++++++++#.",
    ".#++++++++#.",
    ".##########.",
    "............",
    "............" };

/* BLEACH BYPASS: a contrast wedge, black at one end and open at the other */
static const se_icon ic_wedge = {
    "############",
    "#..........#",
    "#+.........#",
    "#++........#",
    "#+++.......#",
    "#++++......#",
    "#+++++.....#",
    "#++++++....#",
    "#+++++++...#",
    "#++++++++..#",
    "#+++++++++.#",
    "############" };

/* ACROS: a disc split down the middle. Black and white, literally */
static const se_icon ic_half = {
    "...######...",
    "..##++..##..",
    ".##+++....#.",
    "##+++++....#",
    "#++++++.....",
    "#++++++.....",
    "#++++++.....",
    "#++++++.....",
    "##+++++....#",
    ".##+++....#.",
    "..##++..##..",
    "...######..." };

/* REALA ACE: a colour chart. The one that tries to be accurate */
static const se_icon ic_chart = {
    "############",
    "#++##++##++#",
    "#++##++##++#",
    "############",
    "#++##++##++#",
    "#++##++##++#",
    "############",
    "#++##++##++#",
    "#++##++##++#",
    "############",
    "............",
    "............" };

/* SOUP 800: a street light, haloed. The whole reason it exists */
static const se_icon ic_lamp = {
    ".....++.....",
    "....+##+....",
    "...+####+...",
    "..+##....##+",
    "..+##....##+",
    "...+####+...",
    ".....##.....",
    ".....##.....",
    ".....##.....",
    ".....##.....",
    "..########..",
    "............" };

/* GOLD: the sun, for golden hour in a box */
static const se_icon ic_sun = {
    "..#..##..#..",
    "...#.##.#...",
    "....####....",
    "..##++++##..",
    "##++++++++##",
    ".#++++++++#.",
    ".#++++++++#.",
    "##++++++++##",
    "..##++++##..",
    "....####....",
    "...#.##.#...",
    "..#..##..#.." };

static const char *const *stock_icon(const char *name)
{
    static const struct { const char *key; const se_icon *art; } t[] = {
        { "VELVIA",     &ic_peaks  },
        { "ASTIA",      &ic_bust   },
        { "PORTRA",     &ic_bust   },
        { "CLASSIC CH", &ic_camera },
        { "CLASSIC N",  &ic_strip  },
        { "NOSTALGIC",  &ic_strip  },
        { "ETERNA",     &ic_clap   },
        { "BLEACH",     &ic_wedge  },
        { "ACROS",      &ic_half   },
        { "REALA",      &ic_chart  },
        { "SOUP 800",   &ic_lamp   },
        { "GOLD",       &ic_sun    },
    };
    int i;
    if (name)
        for (i = 0; i < (int)(sizeof t / sizeof t[0]); i++)
            if (name_is(name, t[i].key)) return *t[i].art;
    return ic_iris;            /* PROVIA, and anything unrecognised */
}

static void draw_icon(se_ui_fb *fb, int x, int y, const char *const *art,
                      int s, uint8_t ink, uint8_t accent)
{
    int r, c;
    for (r = 0; r < SE_ICON_N; r++) {
        const char *row = art[r];
        for (c = 0; c < SE_ICON_N && row[c]; c++) {
            if (row[c] == '.') continue;
            se_ui_fill(fb, x + c * s * 2, y + r * s, s * 2, s,
                       row[c] == '+' ? accent : ink);
        }
    }
}

/* --------------------------------------------------------- the screens */
/*
 * The stock screen, and it is one thing: the name of the film you are
 * about to shoot, as large as the panel will carry.
 *
 * Everything else is support. The neighbours are named small and grey at
 * the margins so the wheel has somewhere to go; the band under the name
 * carries the speed and the colour; the line under that is what the stock
 * does, in the stock's own words.
 */
static void draw_stock(const se_ui *u, se_ui_fb *fb)
{
    int n = u->nstocks, i, cur = 0;
    const char *dsc;
    char buf[64], *d;

    paper_ground(fb);

    if (n <= 0) {
        header(fb, "SOUP", NULL);
        if (!u->ready) {
            big_name(fb, 92, "LOADING", SE_C_MID);
        } else {
            big_name(fb, 80, "NO FILM", SE_C_RED);
            se_ui_text_c(fb, SE_UI_W / 2, 132,
                         "PUT .FS FILES IN ML/FILM ON THE CARD",
                         SE_C_SLATE, 2, 1);
        }
        footer(fb, "DEL CLOSE");
        return;
    }

    cur = (u->stock % n + n) % n;
    dsc = u->stocks[cur].desc ? u->stocks[cur].desc : "";

    /* the header carries the count, which is the only number worth having
     * up there: it says the card was read and how much is on it */
    d = buf;
    d = put_int(d, cur + 1);
    d = put_str(d, " OF ");
    put_int(d, n);
    header(fb, "SOUP", buf);

    /*
     * Arrows at the margins, and nothing else.
     *
     * The neighbouring stocks used to be named here. With thirteen on the
     * card that is three film names across one line, and the eye cannot
     * tell at a glance which of the three it is meant to be reading. The
     * arrows alone say the wheel does something; the ticks below say where
     * in the list you are.
     */
    if (n > 1) {
        se_ui_text(fb, 16, 114, SE_SYM_LEFT, SE_C_MID, 3, 2);
        se_ui_text(fb, SE_UI_W - 32, 114, SE_SYM_RIGHT, SE_C_MID, 3, 2);
    }

    /* the icon and the name as one centred unit, then the band of colour */
    {
        const char *nm = u->stocks[cur].name ? u->stocks[cur].name : "?";
        se_stock_look lk = stock_look(u->stocks[cur].name);
        const char *const *art = stock_icon(u->stocks[cur].name);
        static const int ladder[4] = { 7, 6, 5, 4 };
        const int is = 2, gap = 18;
        int iw = SE_ICON_N * is * 2;           /* 2:1 pixels, see draw_icon */
        int sx = 4, sy, tw, x0, i2;

        /* The name still sets its own size, but the room it has to do it
         * in is what is left after the icon and the gap. */
        for (i2 = 0; i2 < 4; i2++)
            if (se_ui_text_w(nm, ladder[i2]) <= SE_UI_W - 120 - iw - gap) {
                sx = ladder[i2]; break;
            }
        sy = sx > 5 ? 4 : 3;
        tw = se_ui_text_w(nm, sx);
        x0 = (SE_UI_W - (iw + gap + tw)) / 2;

        /* centred on the name's own height, not on the line */
        draw_icon(fb, x0, 54 + (SE_FONT_H * sy - SE_ICON_N * is) / 2,
                  art, is, SE_C_INK, lk.accent);
        se_ui_text(fb, x0 + iw + gap, 54, nm, SE_C_INK, sx, sy);

        speed_band(fb, 108, lk.accent, lk.iso);
    }

    /* what the stock does, in its own words, up to the first colon */
    d = buf;
    for (i = 0; dsc[i] && dsc[i] != ':' && i < 44; i++) *d++ = dsc[i];
    *d = 0;
    se_ui_text_c(fb, SE_UI_W / 2, 154, buf, SE_C_SLATE, 2, 1);

    /*
     * Where you are in the list, as plain ticks. One per stock up to
     * twenty-four, after which they would stop being countable and are
     * dropped -- the "N OF M" in the header still says.
     */
    if (n <= 24) {
        int w = n * 14 - 6, x0 = (SE_UI_W - w) / 2;
        for (i = 0; i < n; i++) {
            if (i == cur) se_ui_fill(fb, x0 + i * 14, 176, 8, 4, SE_C_INK);
            else          se_ui_fill(fb, x0 + i * 14, 178, 8, 2, SE_C_EDGE);
        }
    }

    /*
     * The source belongs on this screen, not only in settings. It is the
     * difference between a frame arriving in seconds and one arriving in
     * minutes, and between Canon's colour and ours.
     */
    /*
     * "SOURCE: RAW", with the value in ink and the label grey.
     *
     * Two colours rather than one sentence. The label is the same every
     * time and the value is the only part that changes, so the eye should
     * land on the value. Centred as a unit, which needs both widths first.
     *
     * Grain is gone from this line: the payload never reads the UI's grain
     * setting -- grain comes from the stock file's own GRAIN line and is
     * part of what a stock IS -- so saying "GRAIN ON" here was reporting a
     * switch that is not wired to anything.
     */
    {
        const char *val = g_src_name[se_ui_source(u)];
        int lw = se_ui_text_w("SOURCE: ", 2);
        int vw = se_ui_text_w(val, 2);
        int x  = (SE_UI_W - (lw + vw)) / 2;
        se_ui_text(fb, x,      194, "SOURCE: ", SE_C_MID, 2, 1);
        se_ui_text(fb, x + lw, 194, val,        SE_C_INK, 2, 1);
    }

    /*
     * What the body actually does, not what se_ui_key_press does. The
     * camera never routes buttons through this state machine -- se_key in
     * the payload calls se_next_stock and se_toggle_source directly -- so
     * this line names the camera's controls.
     */
    footer(fb, "\x01\x02 SOURCE    SET CHOOSE    DEL CLOSE");
}

static void draw_settings(const se_ui *u, se_ui_fb *fb)
{
    int i, cur;
    const int row_y = 48, row_h = 42;

    paper_ground(fb);
    header(fb, "SETTINGS", NULL);

    /* clamp for display: an out-of-range value must still show a selected
     * row rather than silently highlighting nothing */
    cur = SE_CLAMP(u->setting, 0, SE_SET_COUNT - 1);

    for (i = 0; i < SE_SET_COUNT; i++) {
        int y = row_y + i * row_h;
        int sel = (i == cur);
        char label[32], value[48];

        setting_label(i, label);
        setting_value(u, i, value);

        /* the selected row is marked by a bar and by weight, not by a
         * filled block -- reversed type is harder to read, and this screen
         * is nothing but reading */
        if (sel) se_ui_fill(fb, 16, y - 6, 6, 28, SE_C_AMBER);
        se_ui_text(fb, 36, y, label, sel ? SE_C_INK : SE_C_MID, 3, 2);
        se_ui_text(fb, SE_UI_W - 36 - se_ui_text_w(value, 2), y + 4,
                   value, sel ? SE_C_INK : SE_C_MID, 2, 1);
        if (i < SE_SET_COUNT - 1)
            se_ui_fill(fb, 36, y + row_h - 16, SE_UI_W - 72, 1, SE_C_EDGE);
    }

    {
        static const char *help[SE_SET_COUNT] = {
            "SECONDS AND CANON S COLOUR, OR MINUTES AND OURS",
            "OUTPUT SIZE OF THE DEVELOPED JPEG",
            "EACH STOCK CARRIES ITS OWN GRAIN - THIS TURNS IT OFF"
        };
        se_ui_text_c(fb, SE_UI_W / 2, 190, help[cur], SE_C_SLATE, 2, 1);
    }

    footer(fb, "\x01\x02 CHANGE    MENU BACK");
}

static void draw_working(const se_ui *u, se_ui_fb *fb)
{
    int n = u->nstocks;
    const char *nm = (n > 0) ? u->stocks[(u->stock % n + n) % n].name : "?";
    char buf[64], *d;
    const int bar_x = 110, bar_w = SE_UI_W - 220, bar_y = 130, bar_h = 20;
    int filled = (bar_w - 4) * SE_CLAMP(u->percent, 0, 100) / 100;

    paper_ground(fb);
    header(fb, "DEVELOPING", u->file[0] ? u->file : NULL);

    big_name(fb, 54, nm ? nm : "?", SE_C_INK);

    d = buf;
    d = put_str(d, u->stage && u->stage[0] ? u->stage : "WORKING");
    d = put_str(d, "   ");
    d = put_int(d, u->percent);
    put_str(d, "%");
    se_ui_text_c(fb, SE_UI_W / 2, 108, buf, SE_C_SLATE, 2, 1);

    /* a plain bar. It is a progress bar; it does not need to be film */
    plate(fb, bar_x, bar_y, bar_w, bar_h, SE_C_PAPER, SE_C_INK, SE_C_PAPER);
    if (filled > 0)
        se_ui_fill(fb, bar_x + 2, bar_y + 2, filled, bar_h - 4, SE_C_AMBER);

    d = buf;
    d = put_str(d, "FRAME ");
    d = put_int(d, u->done + 1);
    d = put_str(d, " OF ");
    put_int(d, u->total);
    se_ui_text_c(fb, SE_UI_W / 2, 164, buf, SE_C_MID, 2, 1);

    d = buf;
    d = put_str(d, "SOURCE ");
    d = put_str(d, g_src_name[se_ui_source(u)]);
    d = put_str(d, "   ");
    d = put_str(d, g_res_name[setval(u, SE_SET_RES)]);
    se_ui_text_c(fb, SE_UI_W / 2, 190, buf, SE_C_MID, 2, 1);

    footer(fb, "MENU CANCEL");
}

static void draw_done(const se_ui *u, se_ui_fb *fb)
{
    char buf[64], *d;

    paper_ground(fb);
    header(fb, "SOUP", NULL);

    big_name(fb, 62, "DEVELOPED", SE_C_INK);
    se_ui_fill(fb, (SE_UI_W - 300) / 2, 116, 300, 4, SE_C_OLIVE);

    d = buf;
    d = put_int(d, u->done);
    d = put_str(d, u->done == 1 ? " FRAME IN " : " FRAMES IN ");
    d = put_int(d, u->last_seconds);
    put_str(d, " S");
    se_ui_text_c(fb, SE_UI_W / 2, 138, buf, SE_C_INK, 3, 2);

    se_ui_text_c(fb, SE_UI_W / 2, 178,
                 "JPEGS WRITTEN NEXT TO THE RAW FILES", SE_C_SLATE, 2, 1);

    footer(fb, "ANY KEY TO CONTINUE");
}

void se_ui_draw(const se_ui *u, se_ui_fb *fb)
{
    switch (u->screen) {
    case SE_SCREEN_STOCK:    draw_stock(u, fb);    break;
    case SE_SCREEN_SETTINGS: draw_settings(u, fb); break;
    case SE_SCREEN_WORKING:  draw_working(u, fb);  break;
    case SE_SCREEN_DONE:     draw_done(u, fb);     break;
    default:                 paper_ground(fb); break;
    }
}
