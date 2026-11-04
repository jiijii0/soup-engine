/*
 * ui.h - the on-camera menu.
 *
 * The 5D Classic's overlay buffer is 360 bytes x 240 rows, two 4-bit
 * pixels per byte: 720 x 240 pixels, 16 colours. Magic Lantern's own
 * coordinate space is 720 x 480, which the VxWorks bodies halve
 * vertically -- so a pixel in this buffer is displayed twice as TALL as
 * it is wide. Text therefore takes separate x and y scales, and a
 * readable line is typically sx = 2 * sy.
 *
 * All of this is portable C with no Magic Lantern dependency, so the whole
 * menu can be rendered and reviewed on a desktop before it ever runs on
 * the camera. The ML side is then a memcpy into bmp_vram() plus a key map.
 */
#ifndef SE_UI_H_INCLUDED
#define SE_UI_H_INCLUDED

#include "se.h"

#define SE_UI_W      720
#define SE_UI_H      240
#define SE_UI_PITCH  360                      /* bytes per row */
#define SE_UI_FB_SIZE (SE_UI_PITCH * SE_UI_H)

#define SE_FONT_W      5
#define SE_FONT_H      7
#define SE_FONT_GLYPHS 68
extern const uint8_t se_ui_font[SE_FONT_GLYPHS][SE_FONT_W];

/* UI symbols that live past the printable ASCII range in the font table. */
#define SE_SYM_LEFT   "\x01"
#define SE_SYM_RIGHT  "\x02"
#define SE_SYM_BLOCK  "\x03"
#define SE_SYM_BOX    "\x04"

/*
 * The overlay palette, and it is OURS now -- not Magic Lantern's.
 *
 * Sixteen words go to the DIGIC palette registers at 0xC0F14080..0xC0F140BC
 * with a latch at 0xC0F14078, and the format is measured, not assumed:
 *
 *     word = (opacity << 24) | (Y << 16) | (U << 8) | V
 *
 * from ConvertYUVPaletteToPBPalette at 0xFFB0A528 -- opacity 3 is opaque,
 * and an alpha byte of 0x3F or less comes back as 0x00FC0000, which is the
 * transparent entry (0xFFB0A568). U and V are signed with zero neutral,
 * BT.601: U = (B-Y)*0.564, V = (R-Y)*0.713. That was checked against four
 * of ML's own entries -- its red is Y=70 U=-34 V=127 against a computed
 * Y=76 U=-43 V=127, its blue Y=26 U=102 V=-22 against Y=29 U=127 V=-21 --
 * so ML sits at about 80% chroma with white at Y=235, and these do too.
 *
 * ENTRY 15 IS FIXED AT 0x03EB0000 AND MUST STAY THERE. It is not a design
 * choice. set_ml_palette_if_dirty (0x00981C50) decides whether to overwrite
 * the palette by comparing ONE word -- PB_Palette[47] at 0x0002943C, the
 * shadow register for entry 15 -- against 0x03EB0000 (0x00981C6C). Keeping
 * entry 15 at that value and writing it to the shadow means Magic Lantern
 * is permanently satisfied and never puts its own palette back over ours.
 * Change it and ML will fight us for the registers on every draw it makes.
 * docs/ML_DISPLAY.md 3.9, 4.6.
 *
 * Everything else is a warm six-step paper ramp and seven muted accents: a
 * printed look rather than the bold primaries ML needs for a debug overlay.
 * There are no browns or pastels in ML's set at all, which is why the old
 * interface could only ever look like 1987 shelf packaging; this one can
 * look like the box it came in.
 *
 * tools/fb2png.py carries the same RGB values, so a desktop preview is what
 * the panel shows.
 */
#define SE_C_CLEAR    0x00   /* transparent (ML's own 0x00FC0000)       */
#define SE_C_INK      0x01   /* #1E1A17  near-black, warm               */
#define SE_C_SHADOW   0x02   /* #3D362F  deep warm grey                 */
#define SE_C_SLATE    0x03   /* #5E554A  warm grey, dark                */
#define SE_C_MID      0x04   /* #857A6B  warm grey                      */
#define SE_C_EDGE     0x05   /* #ABA092  warm grey, light               */
#define SE_C_PAPER    0x06   /* #E2D9C4  off-white -- the ground        */
#define SE_C_HILITE   0x07   /* #F2EBDA  paper highlight                */
#define SE_C_RUST     0x08   /* #8A3B22  deep rust                      */
#define SE_C_RED      0x09   /* #C04A2E  brick red                      */
#define SE_C_AMBER    0x0A   /* #DC8A32  amber                          */
#define SE_C_GOLD     0x0B   /* #D9BE4A  mustard gold                   */
#define SE_C_OLIVE    0x0C   /* #6B8A4E  olive green                    */
#define SE_C_TEAL     0x0D   /* #357380  teal                           */
#define SE_C_PLUM     0x0E   /* #8E5670  dusty plum                     */
#define SE_C_WHITE    0x0F   /* FIXED at 0x03EB0000 -- see above        */

/* The sixteen words themselves live next to the code that writes them,
 * ml/payload/film.c, because writing them is a camera operation. */

typedef struct { uint8_t *px; } se_ui_fb;

void se_ui_clear (se_ui_fb *fb, uint8_t c);
void se_ui_px    (se_ui_fb *fb, int x, int y, uint8_t c);
void se_ui_fill  (se_ui_fb *fb, int x, int y, int w, int h, uint8_t c);
void se_ui_frame (se_ui_fb *fb, int x, int y, int w, int h, uint8_t c);
int  se_ui_text  (se_ui_fb *fb, int x, int y, const char *s,
                  uint8_t c, int sx, int sy);
int  se_ui_text_w(const char *s, int sx);
int  se_ui_text_c(se_ui_fb *fb, int cx, int y, const char *s,
                  uint8_t c, int sx, int sy);   /* centred on cx */

/* ----------------------------------------------------------- screens */

typedef enum {
    SE_SCREEN_STOCK = 0,
    SE_SCREEN_SETTINGS,
    SE_SCREEN_WORKING,
    SE_SCREEN_DONE,
    SE_SCREEN_COUNT
} se_ui_screen;

typedef enum {
    SE_KEY_LEFT = 0, SE_KEY_RIGHT, SE_KEY_UP, SE_KEY_DOWN,
    SE_KEY_SET, SE_KEY_MENU, SE_KEY_PLAY, SE_KEY_INFO
} se_ui_key;

/*
 * Three settings. Exposure, white balance, grain and the whole colour
 * response live in the stock file, where they belong: a stock is a complete
 * description of a look, not a starting point you then have to tune.
 *
 * SOURCE is the one that changes what the camera actually does:
 *
 *   PREVIEW  grade the full-size JPEG DIGIC already made and embedded in
 *            the CR2. Canon did the demosaic, the white balance and the
 *            colour correction; we own the tone response. Fast, and the
 *            only path the body has ever finished a frame on.
 *
 *   RAW      decode the sensor data ourselves and run the whole pipeline.
 *            Every decision is ours, including the ones Canon makes well.
 *            It costs about 25 MB of card traffic each way through a spool
 *            and minutes rather than seconds.
 *
 * Both develop one frame at a time, right after the shot. RESOLUTION
 * applies to both, as the output size.
 */
enum { SE_SET_SOURCE = 0, SE_SET_RES, SE_SET_GRAIN, SE_SET_COUNT };

enum { SE_SOURCE_PREVIEW = 0, SE_SOURCE_RAW };

/* A stock as the menu sees it. The camera fills this by scanning
 * the ML/FILM directory on the card, so adding a stock needs no rebuild. */
typedef struct {
    const char *name;
    const char *desc;
} se_ui_stock;

typedef struct {
    se_ui_screen screen;

    const se_ui_stock *stocks; /* supplied by the caller; may be empty */
    int nstocks;

    int stock;                 /* index into `stocks`                 */
    int setting;               /* cursor on the settings screen     */
    int value[SE_SET_COUNT];

    /* progress, driven by the engine's se_progress() hook */
    int         percent;
    const char *stage;
    char        file[24];
    int         done, total;

    /* shown on the done screen */
    int         last_seconds;

    /*
     * Has the card been read yet? The screen can open before the watcher
     * has scanned ML/FILM -- deliberately, so DELETE works the moment the
     * camera is on -- and "NO STOCKS, PUT .FS FILES ON THE CARD" would be
     * a lie for the first few seconds. Zero means still looking.
     */
    int         ready;
} se_ui;

void se_ui_init(se_ui *u);
void se_ui_set_stocks(se_ui *u, const se_ui_stock *list, int n);
void se_ui_key_press(se_ui *u, se_ui_key k);
void se_ui_draw(const se_ui *u, se_ui_fb *fb);

/* Translate the UI's settings into engine options. */
int  se_ui_scale(const se_ui *u);
int  se_ui_grain_on(const se_ui *u);
int  se_ui_source(const se_ui *u);     /* SE_SOURCE_PREVIEW or _RAW */

#endif /* SE_UI_H_INCLUDED */
