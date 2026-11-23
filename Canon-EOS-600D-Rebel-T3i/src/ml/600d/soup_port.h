/* soup_port.h - the platform under soup_dev.c, and the per-camera constants.
 *
 * On the camera this is Magic Lantern. With SOUP_HOST it is a few stdio
 * stand-ins with the same names and shapes, so soup_dev.c compiles
 * unchanged on the desktop.
 *
 * This copy is for the EOS 600D, firmware 1.0.2. Every firmware address the
 * module touches lives in the "camera" block below (the 60D module kept two
 * of them hard-coded in soup.c and soup_ui.c). GPL v2. */
#ifndef SOUP_PORT_H
#define SOUP_PORT_H

#ifndef SOUP_HOST
#  include <dryos.h>
#  include <fio-ml.h>
#  include <mem.h>
#else
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#  include <sys/stat.h>
typedef FILE soup_file;
#  define O_RDONLY 0
static inline FILE *FIO_OpenFile(const char *p, int mode) { (void)mode; return fopen(p, "rb"); }
static inline FILE *FIO_CreateFile(const char *p) { return fopen(p, "wb"); }
static inline void  FIO_CloseFile(FILE *f) { fclose(f); }
static inline int   FIO_ReadFile(FILE *f, void *b, size_t n) { return (int)fread(b, 1, n, f); }
static inline int   FIO_WriteFile(FILE *f, const void *b, size_t n) { return (int)fwrite(b, 1, n, f); }
static inline long long FIO_SeekSkipFile(FILE *f, long long off, int whence) { return fseek(f, (long)off, whence) ? -1 : off; }
static inline int   FIO_GetFileSize(const char *p, uint32_t *sz) { struct stat st; if (stat(p, &st)) return -1; *sz = (uint32_t)st.st_size; return 0; }
/* Count what the develop asks for, so the test can report the peak. */
extern size_t soup_host_live, soup_host_peak;
static inline void *soup_host_alloc(size_t n) { size_t *p = malloc(n + 16); if (!p) return 0; p[0] = n; soup_host_live += n; if (soup_host_live > soup_host_peak) soup_host_peak = soup_host_live; return (char *)p + 16; }
static inline void  soup_host_free(void *q) { if (!q) return; size_t *p = (size_t *)((char *)q - 16); soup_host_live -= p[0]; free(p); }
#  define malloc(n)     soup_host_alloc(n)
#  define free(p)       soup_host_free(p)
#  define fio_malloc(n) soup_host_alloc(n)
#  define fio_free(p)   soup_host_free(p)
#endif

/* ------------------------------------------------------------- camera ---- */
/* EOS 600D, firmware 1.0.2. Plain #defines only, so this is safe on the host. */

#define SOUP_CAMERA_NAME  "Canon EOS 600D"   /* EXIF Model if the source has none */
#define SOUP_FW_REQUIRED  "1.0.2"            /* checked against firmware_version  */

/*
 * Canon's own "which photo is playback showing" variables. Magic Lantern does
 * not offer them, so they are read straight from RAM, and they are specific
 * to one firmware. The 60D 1.1.1 values (0x59B74 and 0x2A194) are NOT valid
 * on a 600D, so they are left at 0 here: with 0 the playback toggle says "not
 * set up" and does nothing, which is safe. Everything else in the module works.
 *
 *   SOUP_PLAY_FOLDER_FILE_ADDR  word holding  folder << 16 | file   (100 << 16 | 2278)
 *   SOUP_PLAY_FILE_ADDR         word holding  file    (optional: 0 = skip the cross-check)
 *
 * To find them, build once with -DSOUP_PLAY_PROBE (see soup_play_probe() in
 * soup.c), or step through playback in qemu-eos as was done for the 60D.
 * Then put the addresses here, or pass them with -DSOUP_PLAY_FOLDER_FILE_ADDR=0x...
 */
#ifndef SOUP_PLAY_FOLDER_FILE_ADDR
#  define SOUP_PLAY_FOLDER_FILE_ADDR 0xc680
#endif
#ifndef SOUP_PLAY_FILE_ADDR
#  define SOUP_PLAY_FILE_ADDR 0
#endif

/*
 * 1: SET on the plain shooting screen opens the film picker (as on the 60D,
 * where Canon leaves SET unused there). On the 600D Canon may already use SET
 * on that screen; if it does, build with -DSOUP_SHOOT_SET_OPENS_PICKER=0 and
 * open the picker from Magic Lantern's menu (Shoot > Soup Film, SET).
 */
#ifndef SOUP_SHOOT_SET_OPENS_PICKER
#  define SOUP_SHOOT_SET_OPENS_PICKER 1
#endif

#endif
