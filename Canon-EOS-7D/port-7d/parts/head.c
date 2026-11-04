/* SPDX-License-Identifier: GPL-2.0-only
 * Soup Engine module for the original EOS 7D, firmware 2.0.3, on Magic
 * Lantern lua_fix 7e5067c0a8cf. Uses the exported ML module API only: no
 * ROM addresses and no boot changes. By default (ONE JPEG) a develop
 * leaves exactly one file per frame: the developed JPEG under the camera's
 * own name, replacing Canon's JPEG and the CR2 -- each only after the new
 * file is written and verified. The other output modes keep originals.
 */
#include <module.h>
#include <dryos.h>
#include <menu.h>
#include <fileprefix.h>
#include <bmp.h>
#include <lens.h>
#include <powersave.h>
extern int lv;
extern int menu_redraw_blocked;
int is_pure_play_photo_or_movie_mode(void);
int get_ms_clock(void);
/* exported by ML (7D_203.sym) but not declared in its module headers */
void *memmove(void *dst,const void *src,size_t n);
char *strrchr(const char *s,int c);
int GetFreeMemForAllocateMemory();
int GetFreeMemForMalloc();
void select_menu(char *name,int entry_index);
#include "se.h"
#include "cr2.h"
#include "stock.h"
#include "look.h"
#include "jpeg_dec.h"
#include "jpeg_enc.h"
#include "ui.h"

#define MAX_STOCKS 32
#define HDR_CAP (256*1024)
#define SCRATCH_CAP (768*1024)
#define IO_CAP 8192
#define MAX_PREVIEW (32u*1024u*1024u)
static char stock_paths[MAX_STOCKS][32];
static char stock_names[MAX_STOCKS][SE_STOCK_NAME];
static char stock_descs[MAX_STOCKS][SE_STOCK_DESC];
static const char *stock_choices[MAX_STOCKS];
static int stock_count, selected_stock;
/* Favourites (Q on the stock screen) come first on the dial, in INDEX.TXT
 * order, then the rest. stock_ord is that order, as indexes into stock_*. */
static uint8_t stock_fav[MAX_STOCKS], stock_ord[MAX_STOCKS];
static void order_build(void)
{
    int i,n=0,f;
    for(f=1;f>=0;f--)for(i=0;i<stock_count;i++)if(!!stock_fav[i]==f)stock_ord[n++]=(uint8_t)i;
}
static int stock_pos(int idx){int i;for(i=0;i<stock_count;i++)if(stock_ord[i]==idx)return i;return 0;}
static void stock_step(int dir)
{
    if(stock_count)selected_stock=stock_ord[(stock_pos(selected_stock)+dir+stock_count)%stock_count];
}
static volatile int busy, stop_worker, job_auto;
/* job state shared with the full-screen GUI */
static volatile int cancel_job, progress_pct, job_result; /* 1 some saved, -1 failed/cancelled, -2 all skipped */
static int job_t0, job_ms, job_stock;
static volatile int gui_running;   /* Soup screen owns the display and buttons */
static volatile int gui_auto;      /* Soup screen opened by auto develop: closes itself */
static int gui_open(void);
/* An automatic develop gives way to the photographer: a half-press stops it
 * (the frame is left as it was and tried again later). */
#define JOB_STOP() (stop_worker||ml_shutdown_requested||cancel_job||(job_auto&&get_halfshutter_pressed()))
static char status_text[40]="Ready";
/* Settings, persisted in ML/SETTINGS/SOUP7D.CFG. A job uses the values as
 * they were when it started, never the live ones. */
static int set_res=1;          /* 0 FULL, 1 HALF */
/* Output modes. ONE JPEG and IN PLACE write the developed JPEG under the
 * frame's own camera name, which Canon playback already lists. */
enum {M_ONE, M_INPLACE, M_NEW, M_NEWDEL, M_MLSOUP, M_COUNT};
static int set_mode=M_ONE;
static int set_auto=0;         /* develop each new shot in the background */
static int set_lastn=2;        /* index into lastn_choices: 5 */
static int job_scale=2, job_mode=M_ONE;
/* derived from job_mode for the new-file modes */
static int job_delete=0;       /* NEW FILE + DELETE RAW */
#define CFG_PATH "ML/SETTINGS/SOUP7D.CFG"
#define FIND_LOG "ML/SOUP/FIND.TXT"
static struct menu_entry soup_menu[];

/* Canon FIO handles are integer descriptors, including odd values.
 * FIO_OpenFile normalizes failure to NULL; also reject the raw -1 sentinel.
 * IS_ERROR is for memory/task/directory results, NOT file descriptors. */
static int good_file(FILE *f) { return f != 0 && f != (FILE *)-1; }

/* Diagnostics, ML/SOUP/DIAG.TXT. Deliberately never uses fio_malloc: it
 * must still work when DMA allocations are what is failing. ML's
 * FIO_WriteFile cleans the D-cache for cacheable buffers. */
#define DIAG_PATH "ML/SOUP/DIAG.TXT"
static char diag_buf[1024]; static int diag_len;
static void diag(const char *s)
{
    int n=strlen(s);
    if(diag_len+n+2>=(int)sizeof(diag_buf))return;
    memcpy(diag_buf+diag_len,s,n);diag_len+=n;
    diag_buf[diag_len++]='\r';diag_buf[diag_len++]='\n';diag_buf[diag_len]=0;
}
static void diag_mem(const char *tag)
{
    char t[96];
    snprintf(t,sizeof(t),"%s t=%d malloc_free=%d am_free=%d",tag,get_ms_clock(),
        GetFreeMemForMalloc(),GetFreeMemForAllocateMemory());
    diag(t);
}
static void diag_flush(void)
{
    FILE *f;
    if(!diag_len)return;
    if(is_file(DIAG_PATH))FIO_RemoveFile(DIAG_PATH);
    f=FIO_CreateFile(DIAG_PATH);
    if(good_file(f)){FIO_WriteFile(f,diag_buf,diag_len);FIO_CloseFile(f);}
}
static int read_exact(FILE *f, uint32_t off, void *dst, uint32_t n)
{
    uint32_t done=0;
    if(FIO_SeekSkipFile(f,(int64_t)off,SEEK_SET)!=(int64_t)off)return -1;
    while(done<n){
        int k=FIO_ReadFile(f,(uint8_t*)dst+done,n-done);
        if(k<=0 || (uint32_t)k>n-done)return -1;
        done+=(uint32_t)k;
    }
    return 0;
}
/* >=0 bytes read; -1 open, -2 size/too big, -3 DMA alloc, -4 read */
static int read_text(const char *path,char *dst,int cap)
{
    FILE *f=FIO_OpenFile(path,O_RDONLY);
    uint32_t size=0;void *dma=0;int result=-1;
    if(!good_file(f))return -1;
    result=-2;
    if(FIO_GetFileSize(path,&size)!=0 || size>=(uint32_t)cap)goto out;
    result=-3;
    dma=fio_malloc(size+1);
    if(!dma)goto out;
    result=-4;
    if(read_exact(f,0,dma,size)!=0)goto out;
    memcpy(dst,dma,size);dst[size]=0;result=(int)size;
out:
    if(dma)fio_free(dma);
    FIO_CloseFile(f);return result;
}
/* Replace a small text file (log, settings). Best effort. */
static void write_small_file(const char *path,const char *text)
{
    char *buf;FILE *f;uint32_t len=strlen(text);
    if(!len||len>8192)return;
    buf=fio_malloc(len);
    if(buf)memcpy(buf,text,len);
    else diag("write_small_file: fio_malloc failed, writing direct");
    if(is_file(path))FIO_RemoveFile(path);
    f=FIO_CreateFile(path);
    if(good_file(f)){FIO_WriteFile(f,buf?buf:(char*)text,len);FIO_CloseFile(f);}
    if(buf)fio_free(buf);
}
/* Canon's FIO opens 8.3 names only: base <= 8, extension <= 3. */
static int path_safe(const char *s)
{
    int n=0,base=0,ext=-1;
    while(*s){
        char c=*s++;
        if(c=='.'){if(ext>=0)return 0;ext=0;n++;continue;}
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
             (c>='0'&&c<='9')||c=='_'||c=='-'))return 0;
        if(ext>=0)ext++;else base++;
        n++;
    }
    return n>0&&base>=1&&base<=8&&ext<=3;
}
static void load_stocks(void)
{
    char *index,*text;se_stock *s;void *mem;int n,i,start=0;
    char t[96];int prc;
    mem=malloc(4096+16384+sizeof(se_stock));
    if(!mem){diag("load_stocks: malloc failed");return;}
    index=mem;text=index+4096;s=(se_stock*)(text+16384);
    n=read_text("ML/FILM/INDEX.TXT",index,4096);
    snprintf(t,sizeof(t),"INDEX.TXT rc=%d",n);diag(t);
    if(n<=0){free(mem);return;}
    for(i=0;i<=n&&stock_count<MAX_STOCKS;i++){
        char *line;int got;
        if(index[i]!='\n'&&index[i]!='\r'&&index[i]!=0)continue;
        index[i]=0;line=index+start;start=i+1;
        if(!line[0]||line[0]=='#')continue;
        if(!path_safe(line)){snprintf(t,sizeof(t),"%s skipped: not an 8.3 name",line);diag(t);continue;}
        snprintf(stock_paths[stock_count],32,"ML/FILM/%s",line);
        got=read_text(stock_paths[stock_count],text,16384);
        prc=-99;
        if(got>0){se_stock_init(s);prc=(int)se_stock_parse(text,(size_t)got,s);}
        if(got<=0||prc!=SE_OK){
            snprintf(t,sizeof(t),"%s read=%d parse=%d am_free=%d",line,got,prc,GetFreeMemForAllocateMemory());diag(t);continue;}
        snprintf(stock_names[stock_count],SE_STOCK_NAME,"%s",s->name[0]?s->name:line);
        snprintf(stock_descs[stock_count],SE_STOCK_DESC,"%s",s->desc);
        stock_choices[stock_count]=stock_names[stock_count];stock_count++;
    }
    free(mem);
}
/* Older files carry delete=/out= instead of mode=: those are ignored, so a
 * card upgraded from them starts in the new default, ONE JPEG. The stock is
 * kept by file name, so it survives stocks being added to INDEX.TXT. */
static void load_cfg(void)
{
    char t[640];int n=read_text(CFG_PATH,t,sizeof(t)),i,k;
    order_build();
    if(n<=0)return;
    for(i=0;i<n;i++){
        if(!strncmp(t+i,"res=",4)&&(t[i+4]=='0'||t[i+4]=='1'))set_res=t[i+4]-'0';
        if(!strncmp(t+i,"mode=",5)&&t[i+5]>='0'&&t[i+5]<'0'+M_COUNT)set_mode=t[i+5]-'0';
        if(!strncmp(t+i,"auto=",5)&&(t[i+5]=='0'||t[i+5]=='1'))set_auto=t[i+5]-'0';
        if(!strncmp(t+i,"lastn=",6)&&t[i+6]>='0'&&t[i+6]<='4')set_lastn=t[i+6]-'0';
        if(!strncmp(t+i,"film=",5)){
            char nm[16];int j=0;
            for(k=i+5;j<15&&t[k]&&t[k]!='\r'&&t[k]!='\n';k++)nm[j++]=t[k];
            nm[j]=0;
            for(k=0;k<stock_count;k++)if(!strcmp(stock_paths[k]+8,nm))selected_stock=k;
        }
        if(!strncmp(t+i,"fav=",4)){     /* fav=gold.fs,tmax.fs -- the whole set */
            char nm[16];int j=0;
            memset(stock_fav,0,sizeof(stock_fav));
            for(k=i+4;;k++){
                char c=t[k];
                if(c&&c!=','&&c!='\r'&&c!='\n'){if(j<15)nm[j++]=c;continue;}
                nm[j]=0;
                if(j){int q;for(q=0;q<stock_count;q++)if(!strcmp(stock_paths[q]+8,nm))stock_fav[q]=1;}
                j=0;
                if(c!=',')break;
            }
        }
    }
    order_build();
}
/* Nothing touches the card at boot. Canon's catalogue task indexes every
 * image while the camera starts, and it needs ~71 KB contiguous blocks of
 * the same heap: a module allocating and reading files alongside it was
 * enough to make that fail (Err 70, 2 Nov 2026, MetaCtg Memory.c:186).
 * Stocks and settings load on first use instead. */
static volatile int loaded;
static void load_stocks(void);
static void ensure_loaded(void);
static void save_cfg(void)
{
    char t[640];int i,n;
    /* lengths by strlen: Canon's snprintf is not trusted beyond the text */
    snprintf(t,sizeof(t),"res=%d\r\nmode=%d\r\nauto=%d\r\nlastn=%d\r\nfilm=%s\r\nfav=",set_res,set_mode,set_auto,set_lastn,
        stock_count&&selected_stock<stock_count?stock_paths[selected_stock]+8:"");
    for(i=0;i<stock_count;i++){
        n=strlen(t);
        if(stock_fav[i]&&n<(int)sizeof(t)-24)
            snprintf(t+n,sizeof(t)-n,"%s%s",t[n-1]=='='?"":",",stock_paths[i]+8);
    }
    n=strlen(t);snprintf(t+n,sizeof(t)-n,"\r\n");
    write_small_file(CFG_PATH,t);
}

