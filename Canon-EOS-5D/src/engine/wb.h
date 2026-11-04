/*
 * White balance presets, by name.
 *
 * These lived in cr2.h, which meant stock.c pulled in the whole raw
 * decoder's interface to look up nine strings. They are split out
 * because the stock format needs the names and nothing else: a .fs
 * says WB DAYLIGHT, and that has to parse whether or not anything in
 * the build can open a CR2.
 *
 * The stock editor is the case that forced it. It works on pixels the
 * camera already decoded, so it has no raw path at all, and should not
 * have to carry declarations for one.
 */
#ifndef SE_WB_H
#define SE_WB_H

typedef enum {
    SE_WB_AS_SHOT = 0,
    SE_WB_AUTO,
    SE_WB_MEASURED,
    SE_WB_DAYLIGHT,
    SE_WB_SHADE,
    SE_WB_CLOUDY,
    SE_WB_TUNGSTEN,
    SE_WB_FLUORESCENT,
    SE_WB_FLASH,
    SE_WB_COUNT
} se_wb_preset;

const char *se_wb_name(se_wb_preset p);
int         se_wb_from_name(const char *name);   /* -1 if unknown */

#endif
