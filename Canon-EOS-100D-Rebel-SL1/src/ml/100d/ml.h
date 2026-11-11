/*
 * ml.h - everything this module asks of Magic Lantern, in one include.
 *
 * The module's sources include this and nothing else of Magic Lantern's,
 * so that tools/sim100d can stand in a header of the same name and run
 * the real sources on the desktop against a mock of the camera. Angle
 * brackets, not quotes: a quoted include resolves against this directory
 * first and could never be substituted.
 */
#ifndef SE700_ML_H
#define SE700_ML_H

#include <module.h>
#include <dryos.h>
#include <menu.h>
#include <config.h>
#include <bmp.h>
#include <fio-ml.h>
#include <console.h>
#include <notify_box.h>
#include <lens.h>
#include <fileprefix.h>
#include <powersave.h>
#include <propvalues.h>     /* lv, gui_state */
#include <gui-common.h>     /* display_is_on, is_play_mode, redraw, get_gui_mode, struct event */
#include <shoot.h>          /* is_hdr_bracketing_enabled */

/* The ctx a module callback receives: a raw key callback gets a struct
 * event * in it. 32 bits on the camera; the simulator's copy of this
 * header makes it a uintptr_t so the same source runs on a 64-bit host. */
typedef unsigned int se_ctx_t;

/*
 * The raw button codes the raw key callback compares against. Modules
 * are built platform-neutral, so Magic Lantern's platform/100D.101/gui.h
 * is not on their include path; these are its values, for the one body
 * this module runs on (EOS 100D / Rebel SL1 / Kiss X7). They are what
 * gui_main_task receives. They differ from the 700D's: the cross keys and
 * the touch codes are all shifted.
 */
#ifndef BGMT_PRESS_UP
#define BGMT_MENU              6
#define BGMT_PRESS_ZOOM_IN     0xe
#define BGMT_UNPRESS_ZOOM_IN   0xf
#define BGMT_PRESS_ZOOM_OUT    0x10
#define BGMT_UNPRESS_ZOOM_OUT  0x11
#define BGMT_PRESS_RIGHT       0x26
#define BGMT_UNPRESS_RIGHT     0x27
#define BGMT_PRESS_LEFT        0x28
#define BGMT_UNPRESS_LEFT      0x29
#define BGMT_PRESS_UP          0x2a
#define BGMT_UNPRESS_UP        0x2b
#define BGMT_PRESS_DOWN        0x2c
#define BGMT_UNPRESS_DOWN      0x2d
#define BGMT_TOUCH_1_FINGER    0x6f
#define BGMT_UNTOUCH_1_FINGER  0x70
/* Magic Lantern: two-finger touch never arrives on this body; defined so
 * the shared code compiles, and harmless. */
#define BGMT_TOUCH_2_FINGER    0x76
#define BGMT_UNTOUCH_2_FINGER  0x77
#endif

/* The Q/SET button. The 100D has one button for both, in the middle of the
 * cross keys; Canon sends this code when it is pressed and an ordinary SET
 * release (BGMT_UNPRESS_SET) when it is let go. Magic Lantern names it
 * BGMT_Q_SET and, on the shooting screen, keeps the press to itself to time
 * it; see se_qset_skip in screen.c for what a module then sees. "Last shot"
 * (Q) is the Av (+/-) button, which Magic Lantern reports to modules as Q. */
#define SE_BGMT_Q_SET          0x1d

#endif
