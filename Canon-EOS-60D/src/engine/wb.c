#include "wb.h"
#include "se_libc.h"

static const char *g_wb_names[SE_WB_COUNT] = {
    "As Shot", "Auto", "Measured", "Daylight", "Shade",
    "Cloudy", "Tungsten", "Fluorescent", "Flash"
};

const char *se_wb_name(se_wb_preset p)
{
    int i = (int)p;   /* the enum may be unsigned on ARM */
    return (i >= 0 && i < SE_WB_COUNT) ? g_wb_names[i] : "?";
}

int se_wb_from_name(const char *name)
{
    int i;
    for (i = 0; i < SE_WB_COUNT; i++)
        if (strcmp(name, g_wb_names[i]) == 0) return i;
    /* Lightroom spells a couple of these differently */
    if (strcmp(name, "Custom") == 0)   return SE_WB_AS_SHOT;
    if (strcmp(name, "Tungsten") == 0) return SE_WB_TUNGSTEN;
    return -1;
}
