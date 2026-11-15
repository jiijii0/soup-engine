/* soup_port.h - the platform under soup_dev.c.
 *
 * On the camera this is Magic Lantern. With SOUP_HOST it is a few stdio
 * stand-ins with the same names and shapes, so soup_dev.c compiles
 * unchanged on the desktop. GPL v2. */
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

#endif
