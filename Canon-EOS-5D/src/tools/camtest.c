/*
 * camtest.c - host-side tests for the code that only ever runs on the camera.
 *
 * ml/payload/film.c and ml/payload/libc.c are the two files the host test
 * suite cannot otherwise reach: one implements the raw spool that lets a
 * 25 MB frame stream through a camera that cannot hold it, the other is the
 * payload's own libc. Both are easy to get subtly wrong and impossible to
 * debug on the body, so both are checked here instead.
 *
 * 1. The spool. A frame is developed twice: once from a whole frame in RAM
 *    (the path the sample-driven tests already validate against dcraw) and
 *    once through two spool files using the exact spool_sink and
 *    spool_read_rows logic from film.c. The JPEGs must be byte-identical.
 *    Run it at scale 1 as well as 2 and 4 -- scale 1 is the only mode whose
 *    demosaic re-reads rows it has already seen, so it is the only one that
 *    exercises seeking backwards in the spool.
 *
 * 2. The streaming decode. The camera cannot hold the coded file either --
 *    Canon's heap is 7 MB and a CR2 is about 11 -- so se_cr2_decode_stream
 *    pulls it through an 8 KB window. Decoding the same frame both ways must
 *    produce identical samples, or the window is dropping data at a refill
 *    boundary, which is exactly the kind of fault that would show up as one
 *    corrupt row in a thousand.
 *
 * 3. vsnprintf, against the system one, for every format the engine and the
 *    menu actually use.
 *
 * usage: camtest [file.CR2 [scale]]
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "se.h"
#include "cr2.h"
#include "stock.h"
#include "lut.h"
#include "jpeg_enc.h"
#include "pipeline.h"

int se_vsnprintf(char*,size_t,const char*,va_list);
static int vs(char*b,size_t n,const char*f,...){va_list a;int r;va_start(a,f);r=se_vsnprintf(b,n,f,a);va_end(a);return r;}

/* ---- the streaming reader, as the camera's would behave ---- */
typedef struct { FILE *f; int calls; unsigned long bytes; } rdctx;
static int file_read(void *ctx, uint32_t off, void *dst, uint32_t n)
{
    rdctx *r = ctx;
    r->calls++; r->bytes += n;
    if (fseek(r->f, (long)off, SEEK_SET)) return -1;
    return fread(dst, 1, n, r->f) == n ? 0 : -1;
}

/* ---- the spool, transcribed from ml/payload/film.c ----
 * Including its 32 KB buffering, which is the point: unbuffered it wrote one
 * 4476-byte half-row at a time and managed about 270 KB/s on the body. The
 * buffering is what has to be proved not to drop or duplicate anything. */
#define SPOOL_BUF 32768
typedef struct {
    FILE *h[2];
    int   half, rows, err;
    unsigned char *buf[2];
    int   wlen[2], rbase[2], rlen[2];
} spool;

static int spool_flush(spool *sp, int w)
{
    int n = sp->wlen[w];
    if (!n) return 0;
    sp->wlen[w] = 0;
    return fwrite(sp->buf[w], 1, n, sp->h[w]) == (size_t)n ? 0 : -1;
}
static int spool_put(spool *sp, int w, const void *d, int n)
{
    if (sp->wlen[w] + n > SPOOL_BUF && spool_flush(sp, w) != 0) return -1;
    memcpy(sp->buf[w] + sp->wlen[w], d, n);
    sp->wlen[w] += n;
    return 0;
}
static int spool_get(spool *sp, int w, int off, void *dst, int n)
{
    if (sp->rbase[w] < 0 || off < sp->rbase[w] ||
        off + n > sp->rbase[w] + sp->rlen[w]) {
        if (fseek(sp->h[w], off, SEEK_SET)) return -1;
        sp->rlen[w]  = (int)fread(sp->buf[w], 1, SPOOL_BUF, sp->h[w]);
        sp->rbase[w] = off;
        if (sp->rlen[w] < n) return -1;
    }
    memcpy(dst, sp->buf[w] + (off - sp->rbase[w]), n);
    return 0;
}
static int spool_sink(void *ctx,int row,int col0,int n,const uint16_t*s){
    spool *sp=ctx; int w=(col0>=sp->half)?1:0;
    if(n!=sp->half||(col0!=0&&col0!=sp->half)){sp->err=1;return -1;}
    if(spool_put(sp,w,s,n*2)!=0){sp->err=1;return -1;}
    return 0;
}
static int spool_read_rows(void *ctx,int y,int n,uint16_t*dst){
    spool *sp=ctx; int i,k;
    for(i=0;i<n;i++){ int row=y+i; if(row>=sp->rows) row=sp->rows-1;
        for(k=0;k<2;k++){ int off=row*sp->half*2;
            uint16_t*d=dst+(size_t)i*sp->half*2+(size_t)k*sp->half;
            if(spool_get(sp,k,off,d,sp->half*2)!=0) return -1; } }
    return 0;
}
typedef struct { uint16_t *dst; int pitch; } cr2_mem_sink_t;
static int mem_sink(void *ctx, int row, int col0, int n, const uint16_t *s)
{
    cr2_mem_sink_t *m = ctx;
    memcpy(m->dst + (size_t)row * m->pitch + col0, s, (size_t)n * 2);
    return 0;
}

typedef struct{ unsigned char*b; size_t n,cap; } jbuf;
static int jsink(void*ctx,const uint8_t*d,size_t l){ jbuf*j=ctx;
    if(j->n+l>j->cap){j->cap=(j->n+l)*2;j->b=realloc(j->b,j->cap);}
    memcpy(j->b+j->n,d,l); j->n+=l; return 0; }

int main(int argc,char**argv){
    /* ---------- vsnprintf conformance ---------- */
    int bad=0, cases=0;
    char m[128], r[128];
    #define T(...) do{ cases++; vs(m,sizeof m,__VA_ARGS__); snprintf(r,sizeof r,__VA_ARGS__); \
        if(strcmp(m,r)){bad++;printf("  MISMATCH fmt: mine=%s ref=%s\n",m,r);} }while(0)
    T("%d",0); T("%d",-1); T("%d",2147483647); T("%d",(int)(-2147483647-1));
    T("%u",4294967295u); T("%x",0xdeadbeef); T("%X",0xdeadbeef);
    T("%5d|",42); T("%-5d|",42); T("%05d|",42); T("%05d|",-42); T("%-05d|",42);
    T("%s|","hi"); T("%8s|","hi"); T("%-8s|","hi"); T("%c|",'Z');
    T("%d%%",99); T("%18s%s","Film stock","Velvia"); T("%-18s%s","Film stock","Velvia");
    T("%d done, %d failed",3,1); T("%s %d%%","decode",55); T("%08x",0x1234);
    printf("vsnprintf: %d cases, %d mismatches\n", cases, bad);

    if(argc<2){ return bad?1:0; }

    /* ---------- streaming decode vs in-memory decode ---------- */
    {
        FILE *sf = fopen(argv[1], "rb");
        se_cr2_info si;
        uint16_t *a2, *b2;
        size_t px;
        void *sa;
        FILE *g = fopen(argv[1], "rb");
        long L2; void *whole;
        fseek(g, 0, SEEK_END); L2 = ftell(g); rewind(g);
        whole = malloc(L2); if (fread(whole, 1, L2, g) != (size_t)L2) return 2; fclose(g);
        if (se_cr2_parse(whole, L2, &si) != SE_OK) { puts("parse fail"); return 2; }
        px = (size_t)si.raw_width * si.raw_height;
        a2 = malloc(px * 2); b2 = malloc(px * 2);
        memset(a2, 0xAA, px * 2); memset(b2, 0x55, px * 2);
        sa = malloc(4u << 20);
        {
            se_arena ar2; se_arena_init(&ar2, sa, 4u << 20);
            if (se_cr2_decode(whole, L2, &si, a2, &ar2) != SE_OK) { puts("buf decode fail"); return 2; }
        }
        {
            se_arena ar2; rdctx rc2 = { sf, 0, 0 };
            cr2_mem_sink_t m = { b2, si.raw_width };
            se_arena_init(&ar2, sa, 4u << 20);
            if (se_cr2_decode_stream(file_read, &rc2, &si, mem_sink, &m, &ar2) != SE_OK) {
                puts("stream decode fail"); return 2;
            }
            printf("stream: %d reads, %lu bytes pulled (file is %ld)\n",
                   rc2.calls, rc2.bytes, L2);
        }
        if (!memcmp(a2, b2, px * 2)) printf("STREAM vs RAM decode: IDENTICAL (%zu samples)\n", px);
        else { printf("STREAM vs RAM decode: *** DIFFER ***\n"); bad++; }
        free(a2); free(b2); free(sa); free(whole); fclose(sf);
    }

    /* ---------- spool round trip ---------- */
    FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long L=ftell(f); rewind(f);
    void*buf=malloc(L); if(fread(buf,1,L,f)!=(size_t)L) return 2; fclose(f);
    se_cr2_info info; if(se_cr2_parse(buf,L,&info)!=SE_OK){puts("parse fail");return 2;}
    size_t need=se_develop_scratch_size(&info);
    void*am=malloc(need+(8u<<20)); se_arena ar;

    se_stock st; se_stock_init(&st); strcpy(st.name,"Test");
    se_lut1 tone[3]; se_stock_bake(&st,tone);
    se_dev_opts o; se_dev_opts_default(&o);
    o.scale=(argc>2)?atoi(argv[2]):2; o.tone=tone; o.stock_name="Test"; o.jpeg_quality=88; o.dither=1;

    /* path A: whole frame in RAM */
    uint16_t*frame=malloc((size_t)info.raw_width*info.raw_height*2);
    se_arena_init(&ar,am,need+(8u<<20));
    if(se_cr2_decode(buf,L,&info,frame,&ar)!=SE_OK){puts("decode fail");return 2;}
    se_raw_src sa={frame,0,0};
    jbuf ja={0,0,0};
    se_arena_init(&ar,am,need+(8u<<20));
    se_err rc=se_develop(&info,&sa,0,&o,&ar,jsink,&ja);
    printf("RAM   path: rc=%d jpeg=%zu bytes\n",rc,ja.n);

    /* path B: through the two-file spool */
    spool sp; sp.half=info.raw_width/2; sp.rows=info.raw_height; sp.err=0;
    sp.wlen[0]=sp.wlen[1]=0; sp.rbase[0]=sp.rbase[1]=-1; sp.rlen[0]=sp.rlen[1]=0;
    sp.buf[0]=malloc(SPOOL_BUF); sp.buf[1]=malloc(SPOOL_BUF);
    sp.h[0]=fopen("/tmp/claude-501/fe0.tmp","wb+"); sp.h[1]=fopen("/tmp/claude-501/fe1.tmp","wb+");
    se_arena_init(&ar,am,need+(8u<<20));
    rc=se_cr2_decode_sink(buf,L,&info,spool_sink,&sp,&ar);
    printf("spool decode: rc=%d err=%d\n",rc,sp.err);
    if (spool_flush(&sp,0)||spool_flush(&sp,1)) { puts("spool flush failed"); return 2; }
    fflush(sp.h[0]); fflush(sp.h[1]);
    sp.rbase[0]=sp.rbase[1]=-1;
    se_raw_src sb={0,spool_read_rows,&sp};
    jbuf jb={0,0,0};
    se_arena_init(&ar,am,need+(8u<<20));
    rc=se_develop(&info,&sb,0,&o,&ar,jsink,&jb);
    printf("SPOOL path: rc=%d jpeg=%zu bytes\n",rc,jb.n);

    if(ja.n==jb.n && !memcmp(ja.b,jb.b,ja.n)) printf("RESULT: JPEGs IDENTICAL (%zu bytes)\n",ja.n);
    else { printf("RESULT: *** DIFFER *** (%zu vs %zu)\n",ja.n,jb.n); bad++; }
    return bad?1:0;
}
