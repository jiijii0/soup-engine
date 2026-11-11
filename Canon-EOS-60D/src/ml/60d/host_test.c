/*
 * host_test.c - run soup_dev.c on the desktop.
 *
 *   host_test <in.CR2|in.JPG> <stock.fs|-> <out.jpg>
 *
 * The same soup_dev.c the camera runs, over stdio. Reports time, the
 * output size and the peak memory the develop asked for, which is what has
 * to fit in the 60D's heap. GPL v2.
 */
#include "soup_port.h"
#include "soup_dev.h"
#include <stdarg.h>
#include <time.h>

size_t soup_host_live, soup_host_peak;

void soup_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  log: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int last_pct = -1;
static void progress(void *ctx, const char *stage, int pct)
{
    (void)ctx; (void)stage;
    if (pct / 25 != last_pct / 25) fprintf(stderr, "  %d%%\n", pct);
    last_pct = pct;
}

static int ends_ci(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf), i;
    if (a < b) return 0;
    for (i = 0; i < b; i++) {
        char x = s[a - b + i], y = suf[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    se_stock st;
    clock_t t0;
    int rc;

    if (argc != 4) {
        fprintf(stderr, "usage: host_test <in.CR2|in.JPG> <stock.fs|-> <out.jpg>\n");
        return 2;
    }
    se_stock_init(&st);
    if (strcmp(argv[2], "-")) {
        static char text[16384];
        FILE *f = fopen(argv[2], "rb");
        size_t n;
        if (!f) { perror(argv[2]); return 1; }
        n = fread(text, 1, sizeof text, f);
        fclose(f);
        if (se_stock_parse(text, n, &st) != SE_OK) { fprintf(stderr, "bad stock\n"); return 1; }
    } else {
        strcpy(st.name, "Standard");
    }

    t0 = clock();
    rc = soup_develop(argv[1], ends_ci(argv[1], ".CR2"), argv[3], &st, progress);
    printf("%s via %s: rc=%d (%s), %.2fs host CPU, peak alloc %zu KB, leaked %zu bytes\n",
           argv[1], st.name, rc, se_strerror(rc),
           (double)(clock() - t0) / CLOCKS_PER_SEC, soup_host_peak >> 10, soup_host_live);
    return rc ? 1 : 0;
}
