/*
 * ml.h - everything this module asks of Magic Lantern, in one include.
 *
 * The module's sources include this and nothing else of Magic Lantern's,
 * so that tools/sim700d can stand in a header of the same name and run
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
 * are built platform-neutral, so Magic Lantern's platform/700D.115/gui.h
 * is not on their include path; these are its values, for the one body
 * this module runs on. They are what gui_main_task receives.
 */
#ifndef BGMT_PRESS_UP
#define BGMT_MENU              6
#define BGMT_PRESS_ZOOM_IN     0xe
#define BGMT_UNPRESS_ZOOM_IN   0xf
#define BGMT_PRESS_ZOOM_OUT    0x10
#define BGMT_UNPRESS_ZOOM_OUT  0x11
#define BGMT_PRESS_RIGHT       0x24
#define BGMT_UNPRESS_RIGHT     0x25
#define BGMT_PRESS_LEFT        0x26
#define BGMT_UNPRESS_LEFT      0x27
#define BGMT_PRESS_UP          0x28
#define BGMT_UNPRESS_UP        0x29
#define BGMT_PRESS_DOWN        0x2a
#define BGMT_UNPRESS_DOWN      0x2b
#define BGMT_TOUCH_1_FINGER    0x6b
#define BGMT_UNTOUCH_1_FINGER  0x6c
#define BGMT_TOUCH_2_FINGER    0x72
#define BGMT_UNTOUCH_2_FINGER  0x73
#endif

#endif
