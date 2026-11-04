/*
 * mlmap.h - addresses shared by the 50D payload and the desktop simulator.
 *
 * The numbers come from the 2025-06-20 Magic Lantern build for firmware
 * 1.0.9 (ML/modules/50D_109.sym) and from that autoexec.bin: bmp_vram_real
 * loads bmp_vram_info at 0x24284 and reads the pointer at +20, and
 * display_is_on reads 0x2860. ml_shutdown_requested is the word EngDrvOut
 * tests before it will touch a register.
 */
#ifndef SE_MLMAP_H
#define SE_MLMAP_H

/* Raw BMP VRAM pointer: bmp_vram_info[1].vram2. */
#define CAM_bmp_vram_raw        0x00024298
#define CAM_bmp_vram_real_ptr   CAM_bmp_vram_raw
#define CAM_display_is_on       0x00002860

/* A stable instruction in the ML image, so the simulator can see that the
 * payload did not write over Magic Lantern. copy_and_restart's first word. */
#define ML_menus_display        0x00052F18

/* ml_shutdown_requested in this build. */
#define CAM_ml_shutdown         0x000C9B1C

/*
 * The LCD crop ML draws into. bmp_vram_real() returns a pointer at the
 * top-left of a 720x480 window inside a 960-wide buffer. Pitch stays 960.
 * The UI itself is still 720x240 4-bit; the blit spreads it.
 */
#define ML_BMP_PITCH            960
#define ML_BMP_ROWS             480
#define ML_BMP_BYTES            (ML_BMP_PITCH * ML_BMP_ROWS)
#define CAM_BMP_CROP_X          120
#define CAM_BMP_CROP_Y          30
#define CAM_BMP_INSET           (CAM_BMP_CROP_Y * ML_BMP_PITCH + CAM_BMP_CROP_X)

/*
 * LCD_Palette, 256 entries. Each entry is three words: register, unused,
 * shadow value. alter_bitmap_palette_entry writes EngDrvOut(entry*3) and
 * EngDrvOut(entry*3 + 0x300). Our marker is the shadow of entry 6, which
 * nothing in this image treats as a dirty flag.
 */
#define CAM_LCD_PALETTE         0x000242C0
#define CAM_LCD_PAL_REG(k)      (CAM_LCD_PALETTE + ((k) * 3) * 4)
#define CAM_LCD_PAL_REG2(k)     (CAM_LCD_PALETTE + ((k) * 3 + 0x300) * 4)
#define CAM_PB_PAL_VAL(k)       (CAM_LCD_PALETTE + ((k) * 3 + 2) * 4)
#define CAM_PB_PALETTE          CAM_LCD_PALETTE
#define CAM_PB_PAL_MINE         CAM_PB_PAL_VAL(6)
#define SE_PAL_MINE_INDEX       6

/* Kept so a build that still mentions the old latch name compiles in the
 * simulator. The 50D payload does not read it; shutdown is CAM_ml_shutdown. */
#define CAM_gui_bmp_ready       0x00002864
#define SE_PALETTE_MARK         0x03EB0000

#define ML_PALETTE_N            16

#endif /* SE_MLMAP_H */
