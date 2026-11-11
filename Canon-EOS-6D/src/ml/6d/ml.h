/*
 * ml.h - everything this module asks of Magic Lantern, in one include.
 *
 * The module's sources include this and nothing else of Magic Lantern's,
 * so that tools/sim6d can stand in a header of the same name and run
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
 * are built platform-neutral, so Magic Lantern's platform/6D.116/gui.h
 * is not on their include path; these are its values, for the one body
 * this module runs on. They are what gui_main_task receives.
 *
 * The 6D differs from the 700D in ways the key code in screen.c has to
 * respect:
 *   - the four arrows are one 8-way multi-controller, with diagonals,
 *     and every one of its releases is the single code BGMT_UNPRESS_UDLR;
 *   - the zoom-in button has its own press and release (0x12, 0x13);
 *   - there is no zoom-out button and no touch screen, so none of those
 *     codes exist here, and the code that handled them is compiled out.
 */
#ifndef BGMT_PRESS_UP
#define BGMT_MENU              0x6
#define BGMT_PRESS_ZOOM_IN     0x12
#define BGMT_UNPRESS_ZOOM_IN   0x13
#define BGMT_PRESS_UP          0x2e
#define BGMT_PRESS_UP_RIGHT    0x2f
#define BGMT_PRESS_RIGHT       0x30
#define BGMT_PRESS_DOWN_RIGHT  0x31
#define BGMT_PRESS_DOWN        0x32
#define BGMT_PRESS_LEFT        0x34
#define BGMT_PRESS_UP_LEFT     0x35
#define BGMT_PRESS_DOWN_LEFT   0x1c
#define BGMT_UNPRESS_UDLR      0x36
#endif

#endif
