/*
 * mlmap.h - the map of Magic Lantern's 2012 image, shared by the camera
 * header and the desktop simulator's.
 *
 * These are plain constants, so both sides include this one file rather than
 * each carrying its own copy. The copies are what caused the last round of
 * lost time: a four-byte error in the Debug entry layout lived in the notes
 * and in the payload at once, and the simulator -- which had its own copy of
 * the same numbers -- agreed with it.
 *
 * The simulator reaches this with a relative path on purpose. It is not on
 * the simulator's include path, and spelling it out means neither side can
 * quietly pick up a different file of the same name.
 */
#ifndef SE_MLMAP_H
#define SE_MLMAP_H

/* ------------------------------------------------- the BMP overlay */
/*
 * What is left of Magic Lantern in this project's screen: two addresses and
 * the geometry.
 *
 * Everything else here described how to draw from inside ML's menu redraw
 * -- the idle buffer, the idle flag, bmp_puts, the fonts, the 52-byte tab
 * struct, the nine Debug entries and their callback slots. The soup engine
 * does not use any of it. It ASKS Canon for the panel, the way ML's
 * menu_open does -- a fake MENU press through GUI_Control at 0x0098728C --
 * reasserts the palette before every frame, and blits its own frame
 * straight into the buffer below. It calls nothing in ML's image.
 *
 * Neither of these two is ML's. Both are Canon's, and docs/ML_DISPLAY.md
 * 4.3 says what they are:
 *
 *   0x00029328 is the VALUE field of a shadow-register struct. The
 *   {RAM, register} pair table at 0xFFB6AF50 maps 0x00029320 to register
 *   0xC0F140D0 -- the bitmap layer's base-address register -- and each
 *   shadow is { u32 *reg; int dirty; u32 value; }, so the value is at +8.
 *   SetParameterToBitmapDisplayDevice (0xFFB0A888) is the flush. That is
 *   why nothing in either image loads this address as a literal, and why
 *   it reads zero while the display is down.
 *
 *   0x0000FCA0 is TurnOnDisplay's own flag, set to 1 at 0xFFB09210. ML
 *   gave it the name; the ROM confirms it means exactly that.
 *
 * ML's own bmp_idle_copy writes 86,400 bytes to exactly the same place
 * (0x00980AD8), which is what makes it as trustworthy as ML's menu was,
 * and it is range-checked before anything is written through it anyway.
 *
 * Canon publishes the pointer while the panel is dark -- the camera
 * reported "displayon=0 vram=0x105890C0" -- so the pointer being valid is
 * not sufficient. Both tests, which is what ML does in five places.
 */
#define CAM_bmp_vram_real_ptr   0x00029328   /* the buffer on the panel     */
#define CAM_display_is_on       0x0000FCA0   /* ML's DISPLAY_IS_ON for 5DC  */

/*
 * One function of ML's, and only so the simulator can check we did not
 * overwrite it: menus_display is the first thing the patch tool would
 * damage if a stub ever grew past its budget.
 */
#define ML_menus_display        0x00985A8C

/* ------------------------------------------------- shutting down */
/*
 * ml_shutdown_requested. Every one of Magic Lantern's nine tasks loops on
 * this and ours did not.
 *
 * menu_redraw_task's TASK_LOOP is literally `while (!MEM(0x009C84E0))` --
 * 0x00986C8C loads it, compares with zero and branches back. It is also
 * the FIRST guard in EngDrvOut (0x00989118), which is how every one of
 * ML's palette writes stops at shutdown, and in fake_simple_button
 * (0x00988418). Twenty-three literal references across the image.
 *
 * Why it matters to us: at power-off Canon tears the bitmap screen down --
 * 0xFFA279B0 calls UndisplayPhysicalScreen (0xFFB0B240) and then
 * DeletePhysicalVram (0xFFB0B0E4), which FREES the 86,404-byte buffer
 * MEM(0x00029328) points at. A screen task still blitting 86,400 bytes
 * into it ten times a second is writing into freed heap, and a watcher
 * still walking the card is holding the card open. Either will stop a
 * shutdown finishing.
 */
#define CAM_ml_shutdown         0x009C84E0

/*
 * Does Canon's bitmap screen exist right now?
 *
 * The latch gui_create_bitmap_screen (0xFFA2521C) sets to 1 at 0xFFA25300,
 * immediately after DisplayPhysicalScreen returns -- so it means the VRAM
 * has been allocated by CreatePhysicalVram AND the layer has been shown
 * AND the BmpDDev is live. The teardown at 0xFFA279B0 clears it back to
 * zero before it frees the buffer (0xFFA279F4).
 *
 * So it is exactly the precondition for touching MEM(0x00029328): true
 * from bring-up to teardown and false either side. Better than a timer,
 * which is what used to stand in for it.
 */
#define CAM_gui_bmp_ready       0x0000F3A8
#define ML_BMP_PITCH            360
#define ML_BMP_ROWS             240
#define ML_BMP_BYTES            (ML_BMP_PITCH * ML_BMP_ROWS)

/*
 * The geometry is Canon's own, not a measurement of ML's behaviour.
 * CreatePhysicalVram (0xFFB0AFC0) fills its three out-parameters with 720,
 * 240 and 720 at 0xFFB0B040..0xFFB0B060 and then allocates 0x15184 bytes:
 * 86,400 of 4-bit pixels at 360 bytes a row, plus four of slack.
 * docs/ML_DISPLAY.md 4.1.
 */

/* ------------------------------------------------- the overlay palette */
/*
 * Canon puts its own palette back whenever the panel is turned on, so ours
 * has to be reasserted on every draw rather than once.
 *
 * TurnOnDisplay walks the CBR list at MEM(0x00029318) with r0 = 0
 * (0xFFB09280). One of those callbacks is
 * NotifyTurnOnDisplayToBitmapDisplayDevice (0xFFB0AC74 -- registered, not
 * called: its address is a literal at 0xFFB0AEA0 inside
 * InitializeBitmapDisplayDevice, which is why xref finds no branch to it).
 * It calls SelectParameter (0xFFB0A95C), which rebuilds all sixteen palette
 * shadows from Canon's YUV table at MEM(0x00029450), and then
 * SetParameterToBitmapDisplayDevice (0xFFB0A888), which flushes every dirty
 * shadow to its register. Entries whose alpha byte is <= 0x3F come back out
 * of ConvertYUVPaletteToPBPalette as 0x00FC0000 -- transparent (0xFFB0A568).
 *
 * Magic Lantern checks in two places for this reason: on EVERY call to
 * bmp_vram (set_ml_palette_if_dirty, 0x00980BC0) and again as
 * menu_redraw_do's tail call (0x00986BC4). docs/ML_DISPLAY.md 4.6, 4.7.
 */
/*
 * The palette shadow registers, from the {RAM, register} pair table at
 * 0xFFB6AF50 and the flush loop at 0xFFB0A888. Each shadow is twelve bytes,
 * { u32 *reg; int dirty; u32 value; }, so entry 15's value is
 * 0x00029434 + 8 -- and that is the word set_ml_palette_if_dirty compares
 * at 0x00981C6C. docs/ML_DISPLAY.md 4.3.
 */
#define CAM_PB_PALETTE          0x00029380   /* 16 x 12-byte shadows      */
#define CAM_PB_PAL_VAL(k)       (CAM_PB_PALETTE + (k) * 12 + 8)
#define CAM_PB_PALETTE_DIRTY    CAM_PB_PAL_VAL(15)  /* 0x0002943C: ML's  */

/*
 * OUR marker, in a shadow word Magic Lantern never touches.
 *
 * ML stamps entry 15's value word and tests it (0x00981C6C). Entry 15 is
 * pinned to ML's own 0x03EB0000 so that test stays satisfied and ML never
 * rewrites the palette -- which means that word cannot also be OUR flag:
 * it reads "already ours" when the hardware is holding ML's palette. That
 * mistake shipped once and the whole interface came up red.
 *
 * Entry 6 is PAPER, the background, and nothing but us and Canon ever
 * writes its shadow. Canon's SelectParameter (0xFFB0A95C) rewrites all
 * sixteen value words from its own YUV table whenever the palette is
 * rebuilt, so the moment Canon takes the palette back this word stops
 * matching and we put ours in again. That is ML's own mechanism, applied
 * to a word ML does not use.
 *
 * It has to be a RAM shadow and not the register, because THE PALETTE
 * REGISTERS ARE NEVER READ BY ANYONE. Across the whole 8 MB ROM the
 * sixteen addresses 0xC0F14080..0xC0F140BC appear as literals in exactly
 * one place -- the {shadow, register} descriptor table at 0xFFB6AF50,
 * which SetParameterToBitmapDisplayDevice uses only to WRITE. Canon's own
 * read-back, GetYUVPaletteFromDisplayDevice (0xFFB0BC28), is
 * memcpy(dst, 0x00029450, 64) -- it reads RAM. ML's dirty test reads RAM.
 * Reading them is an unprecedented hardware interaction on this body and
 * is not something to do on the path between a button and the screen.
 */
#define CAM_PB_PAL_MINE         CAM_PB_PAL_VAL(6)   /* 0x000293D0        */
#define SE_PAL_MINE_INDEX       6
#define SE_PALETTE_MARK         0x03EB0000   /* what ML writes there,     */
                                             /* = palette[15], 0x00981C68 */

/*
 * ML's sixteen palette words live in its image, so both the payload and the
 * simulator read them from the binary instead of carrying a copy. A copy is
 * what cost the last round: the Debug entry layout was wrong in the notes
 * AND in the simulator's own duplicate of it, so they agreed.
 *
 * set_ml_palette loads this address at 0x00981BD8 and writes the sixteen
 * words to 0xC0F14080..0xC0F140BC, bound 0xC0F140C0 at 0x00981C04.
 */
#define ML_palette              0x009A2BD0
#define ML_PALETTE_N            16

#endif /* SE_MLMAP_H */
