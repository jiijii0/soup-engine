/* Bare-metal ARM harness: the camera's develop pipeline (decode, look, encode)
 * on a JPEG in memory, for instruction counting under Unicorn. */
#include <stdarg.h>
#include "se.h"
#include "stock.h"
#include "look.h"
#include "jpeg_dec.h"
#include "jpeg_enc.h"
uint8_t  in_jpg[1 << 20];
char     stock_txt[16384];
uint32_t in_len, stock_len, out_len, out_hash;
uint8_t  out_buf[1 << 20];
static uint8_t heap[6 << 20];
void *memcpy(void *d, const void *s, size_t n){uint8_t*a=d;const uint8_t*b=s;while(n--)*a++=*b++;return d;}
void *memset(void *d, int c, size_t n){uint8_t*a=d;while(n--)*a++=(uint8_t)c;return d;}
void *memmove(void *d, const void *s, size_t n){uint8_t*a=d;const uint8_t*b=s;if(a<b)while(n--)*a++=*b++;else{a+=n;b+=n;while(n--)*--a=*--b;}return d;}
size_t strlen(const char *s){size_t n=0;while(s[n])n++;return n;}
int strcmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
int strncmp(const char*a,const char*b,size_t n){while(n&&*a&&*a==*b){a++;b++;n--;}return n?(unsigned char)*a-(unsigned char)*b:0;}
int vsnprintf(char *buf, size_t n, const char *f, va_list ap){if(n)buf[0]=0;return 0;}
typedef struct {se_jpeg *enc; se_look_run *look;} ctx;
static int wr(void *p, const uint8_t *d, size_t n){if(out_len+n>sizeof out_buf)return -1;memcpy(out_buf+out_len,d,n);out_len+=n;return 0;}
static int emit(void *p, const uint8_t *row, int w){return se_jpeg_write_rows(((ctx*)p)->enc,row,1)==SE_OK?0:-1;}
static int rows(void *p, const uint8_t *rgb, int n, int w){return se_look_rows(((ctx*)p)->look,rgb,n,emit,p)==SE_OK?0:-1;}
int bench(void)
{
    se_stock st; se_look lk; se_arena a, la; se_jdec_info ji; ctx c; uint8_t *strip; size_t ls; uint32_t i, h = 2166136261u;
    se_stock_init(&st);
    if (se_stock_parse(stock_txt, stock_len, &st) != SE_OK) return -1;
    if (se_jpeg_dec_info(in_jpg, in_len, &ji) != SE_OK) return -2;
    se_look_from_stock(&st, 2, &lk);          /* HALF: grain and glow sized as on the camera */
    ls = se_look_fit(&lk, ji.width, SE_LOOK_BUDGET);
    se_arena_init(&a, heap, 4 << 20); se_arena_init(&la, heap + (4 << 20), ls);
    c.enc = SE_NEW(&a, se_jpeg, 1); strip = SE_NEW(&a, uint8_t, (size_t)8 * ji.width * 3);
    if (se_look_begin(&c.look, &lk, ji.width, &la) != SE_OK) return -3;
    if (se_jpeg_begin(c.enc, ji.width, ji.height, 88, strip, 0, wr, &c) != SE_OK) return -4;
    if (se_jpeg_decode_scaled(in_jpg, in_len, 1, &a, rows, &c, &ji) != SE_OK) return -5;
    if (se_look_end(c.look, emit, &c) != SE_OK || se_jpeg_end(c.enc) != SE_OK) return -6;
    for (i = 0; i < out_len; i++) { h ^= out_buf[i]; h *= 16777619u; }
    out_hash = h;
    return 0;
}
volatile int result;
__attribute__((noinline, used)) void halt(void) { for (;;); }
__attribute__((section(".text.start"))) void _start(void) { result = bench(); halt(); }
