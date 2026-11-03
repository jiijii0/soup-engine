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

/* ============================================================ frames ====
 * One scan of the shooting folder (plus ML/SOUP) gives every frame number
 * with its CR2/JPG and whether Soup has already developed it. ML's reported
 * prefix is unusable on this body (it stays "IMG_"), so files are matched by
 * number with the prefix ignored. More than one CR2 (or JPG) with one number
 * is ambiguous and that frame is refused rather than guessed.
 *
 * Outputs: SOUP####.JPG with the NEXT FREE number, because Canon playback
 * hides a JPG that shares its number with another image (DCF: one number,
 * one image). The source frame is in the output's EXIF Software tag as
 * "#nnnn". Older outputs (SOUP/SOU2..SOU9 with the source's own number and
 * no "#") still count, by their number. Canon only learns about our numbers
 * at its next boot, so it may reuse one for a shot meanwhile; the scan then
 * renames OUR file to a free number. Camera files are never renamed.
 *
 * In place (ONE JPEG / IN PLACE): the develop is written to SOUI####.TMP,
 * then Canon's JPG is removed and the TMP renamed to the JPG's name. A
 * developed JPG is known by "SOUP ENGINE" in its EXIF and is never used as
 * a source again. If the camera stopped between the remove and the rename,
 * the next scan finishes the rename.
 */
#define MAX_FRAMES 4096
#define F_CR2 1
#define F_JPG 2
#define F_DUP 4
#define F_TMP 8            /* SOUI####.TMP: an in-place develop not yet renamed */
typedef struct {uint16_t num;uint8_t flags,done;char cr2p[4],jpgp[4];} frame_ent;
static frame_ent *frames;
static int nframes, frames_overflow;
#define MAX_OUTS 1024
typedef struct {uint16_t num;uint8_t in_ml,ver;} out_ent;
static out_ent *outs;               /* lives in the same allocation as frames */
static int nouts, max_cam, max_ml;  /* highest numbers in use, camera folder / ML/SOUP */
static volatile int gui_task_alive;
/* The index is rebuilt on every use; holding 48 KB of Canon's heap between
 * uses is what Err 70 punishes. Freed by the GUI task on exit if no job runs,
 * else by the job task once the GUI task has fully exited. */
static void frames_release(void)
{
    frame_ent *f=frames;
    if(!f)return;
    frames=0;nframes=0;outs=0;nouts=0;
    free(f);
}
static char frames_dir[64];

static char up_ascii(char c){return (c>='a'&&c<='z')?(char)(c-32):c;}
static int parse_dcim_name(const char *n,int *num,char ext[4])
{
    int i,v=0;
    if(strlen(n)!=12||n[8]!='.')return 0;
    for(i=0;i<4;i++){char c=up_ascii(n[i]);if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))return 0;}
    for(i=4;i<8;i++){if(n[i]<'0'||n[i]>'9')return 0;v=v*10+(n[i]-'0');}
    for(i=0;i<3;i++)ext[i]=up_ascii(n[9+i]);
    ext[3]=0;*num=v;return v>=1;
}
/* 1 for SOUP, 2..9 for SOU2..SOU9, 0 for anything else */
static int soup_version(const char *n)
{
    if(up_ascii(n[0])!='S'||up_ascii(n[1])!='O'||up_ascii(n[2])!='U')return 0;
    if(up_ascii(n[3])=='P')return 1;
    if(n[3]>='2'&&n[3]<='9')return n[3]-'0';
    return 0;
}
static int frame_lookup(int num)
{
    int lo=0,hi=nframes-1;
    while(lo<=hi){int m=(lo+hi)/2;if(frames[m].num==num)return m;if(frames[m].num<num)lo=m+1;else hi=m-1;}
    return -1-lo;   /* insertion point, encoded */
}
static frame_ent *frame_get(int num)
{
    int at=frame_lookup(num);
    if(at>=0)return &frames[at];
    if(nframes>=MAX_FRAMES){frames_overflow=1;return 0;}
    at=-1-at;
    if(at<nframes)memmove(&frames[at+1],&frames[at],(size_t)(nframes-at)*sizeof(frame_ent));
    memset(&frames[at],0,sizeof(frame_ent));frames[at].num=(uint16_t)num;nframes++;
    return &frames[at];
}
static int scan_dir(const char *dir,int outputs_only)
{
    struct fio_file file;struct fio_dirent *d;int n,v;char ext[4];frame_ent *e;
    d=FIO_FindFirstEx(dir,&file);
    if(IS_ERROR(d))return -1;          /* directory handle: IS_ERROR is right here */
    do{
        if(stop_worker||ml_shutdown_requested)break;
        if(!file.name[0]||file.name[0]=='.'||(file.mode&ATTR_DIRECTORY))continue;
        if(!parse_dcim_name(file.name,&n,ext))continue;
        if(outputs_only){if(n>max_ml)max_ml=n;}else if(n>max_cam)max_cam=n;
        if(!outputs_only&&!strcmp(ext,"TMP")&&up_ascii(file.name[3])=='I'&&soup_version(file.name)==0&&
           up_ascii(file.name[0])=='S'&&up_ascii(file.name[1])=='O'&&up_ascii(file.name[2])=='U'){
            if((e=frame_get(n)))e->flags|=F_TMP;
            continue;
        }
        if(strcmp(ext,"CR2")&&strcmp(ext,"JPG"))continue;
        v=soup_version(file.name);
        if(v&&!strcmp(ext,"JPG")){
            if(nouts<MAX_OUTS){outs[nouts].num=(uint16_t)n;outs[nouts].in_ml=(uint8_t)outputs_only;outs[nouts].ver=(uint8_t)v;nouts++;}
            continue;
        }
        if(outputs_only)continue;
        if(!(e=frame_get(n)))continue;
        if(!strcmp(ext,"CR2")){if(e->flags&F_CR2)e->flags|=F_DUP;else{e->flags|=F_CR2;memcpy(e->cr2p,file.name,4);}}
        else{if(e->flags&F_JPG)e->flags|=F_DUP;else{e->flags|=F_JPG;memcpy(e->jpgp,file.name,4);}}
    }while(FIO_FindNextEx(d,&file)==0);
    FIO_FindClose(d);
    return 0;
}
static void out_path(const out_ent *o,int num,char *out,int cap,const char *ext)
{
    if(o->ver<=1)snprintf(out,cap,"%s/SOUP%04d.%s",o->in_ml?"ML/SOUP":frames_dir,num,ext);
    else snprintf(out,cap,"%s/SOU%d%04d.%s",o->in_ml?"ML/SOUP":frames_dir,o->ver,num,ext);
}
/* Source frame recorded in an output's EXIF ("SOUP ENGINE - <stock> #nnnn"),
 * or -1 for an older output without one. Only the first 1 KB is read. */
static int read_src(const char *path,uint8_t *buf,int cap)
{
    FILE *f;uint32_t size=0,n;int i,v;
    if(FIO_GetFileSize(path,&size)!=0||size<64)return -1;
    n=size<(uint32_t)cap?size:(uint32_t)cap;
    f=FIO_OpenFile(path,O_RDONLY);if(!good_file(f))return -1;
    if(read_exact(f,0,buf,n)!=0){FIO_CloseFile(f);return -1;}
    FIO_CloseFile(f);
    for(i=0;i+11<(int)n;i++){
        if(memcmp(buf+i,"SOUP ENGINE",11))continue;
        for(;i+5<(int)n&&buf[i];i++){
            if(buf[i]=='#'&&buf[i+1]>='0'&&buf[i+1]<='9'&&buf[i+2]>='0'&&buf[i+2]<='9'&&
               buf[i+3]>='0'&&buf[i+3]<='9'&&buf[i+4]>='0'&&buf[i+4]<='9'){
                v=(buf[i+1]-'0')*1000+(buf[i+2]-'0')*100+(buf[i+3]-'0')*10+(buf[i+4]-'0');
                return v>=1?v:-1;
            }
        }
        return -1;
    }
    return -1;
}
/* 1 if the file's first `cap` bytes carry our EXIF Software tag. */
static int soup_marked(const char *path,uint8_t *buf,int cap)
{
    FILE *f;uint32_t size=0,n;int i;
    if(FIO_GetFileSize(path,&size)!=0||size<64)return 0;
    n=size<(uint32_t)cap?size:(uint32_t)cap;
    f=FIO_OpenFile(path,O_RDONLY);if(!good_file(f))return 0;
    i=read_exact(f,0,buf,n);FIO_CloseFile(f);
    if(i!=0)return 0;
    for(i=0;i+11<=(int)n;i++)if(!memcmp(buf+i,"SOUP ENGINE",11))return 1;
    return 0;
}
/* A whole JPEG: SOI first, EOI last. */
static int jpeg_complete(const char *path)
{
    FILE *f;uint32_t size=0;uint8_t b[4];int ok=0;
    if(FIO_GetFileSize(path,&size)!=0||size<4)return 0;
    f=FIO_OpenFile(path,O_RDONLY);if(!good_file(f))return 0;
    if(read_exact(f,0,b,2)==0&&read_exact(f,size-2,b+2,2)==0)
        ok=b[0]==0xFF&&b[1]==0xD8&&b[2]==0xFF&&b[3]==0xD9;
    FIO_CloseFile(f);return ok;
}
static void frame_path(const frame_ent *e,int cr2,char *out,int cap);
static void inplace_tmp(int num,char *out,int cap){snprintf(out,cap,"%s/SOUI%04d.TMP",frames_dir,num);}

/* Next unused output number. Camera folder: above everything in the folder
 * AND above Canon's counter (the last shot it took). -1 when past 9999. */
static int next_output(int in_ml,char *out,char *tmp,int cap)
{
    struct card_info *card=get_shooting_card();
    int n=in_ml?max_ml:max_cam;
    const char *dir=in_ml?"ML/SOUP":frames_dir;
    if(!in_ml&&card&&card->file_number>n)n=card->file_number;
    for(n++;n<=9999;n++){
        snprintf(out,cap,"%s/SOUP%04d.JPG",dir,n);snprintf(tmp,cap,"%s/SOUP%04d.TMP",dir,n);
        if(!is_file(out)&&!is_file(tmp)){if(in_ml)max_ml=n;else max_cam=n;return n;}
    }
    return -1;
}
/* Rebuild the index for the current shooting folder. Never while a job runs. */
static int index_build(void)
{
    int i,j,at,src;uint8_t *buf;char p[96],q[96],tq[96];
    if(!frames){
        frames=malloc(MAX_FRAMES*sizeof(frame_ent)+MAX_OUTS*sizeof(out_ent));if(!frames)return -1;
        outs=(out_ent*)(frames+MAX_FRAMES);
    }
    nframes=0;frames_overflow=0;nouts=0;max_cam=0;max_ml=0;
    snprintf(frames_dir,sizeof(frames_dir),"%s",get_dcim_dir());
    if(scan_dir(frames_dir,0)!=0)return -1;
    scan_dir("ML/SOUP",1);             /* outputs kept there count as developed */
    buf=fio_malloc(1024);
    for(i=0;i<nouts;i++){
        out_ent *o=&outs[i];
        out_path(o,o->num,p,sizeof(p),"JPG");
        src=buf?read_src(p,buf,1024):-1;
        if(src<0)src=o->num;           /* older output: its own number is the source */
        /* Canon reused our number for a real shot: move OUR file out of the way */
        if(!o->in_ml&&src!=o->num&&(at=frame_lookup(o->num))>=0&&(frames[at].flags&(F_CR2|F_JPG))){
            int nn=next_output(0,q,tq,sizeof(q));
            if(nn>0){
                out_ent t=*o;t.ver=1;out_path(&t,nn,q,sizeof(q),"JPG");
                if(FIO_RenameFile(p,q)==0){char d_[224];snprintf(d_,sizeof(d_),"renamed %s -> %s (camera reused the number)",p,q);diag(d_);diag_flush();o->num=(uint16_t)nn;o->ver=1;}
            }
        }
        if((at=frame_lookup(src))>=0)frames[at].done=1;
    }
    /* Interrupted in-place swaps: the TMP is the develop, the JPG is gone.
     * Finish the rename -- only for a complete JPEG; a cut-off one is a
     * develop that died writing (a RAW-only frame still has its CR2). A
     * TMP beside a JPG never got as far as the swap: the JPG is the
     * original. */
    for(i=0;i<nframes;i++){
        frame_ent *e=&frames[i];
        if(!(e->flags&F_TMP))continue;
        inplace_tmp(e->num,tq,sizeof(tq));
        if(!(e->flags&F_JPG)&&jpeg_complete(tq)){
            if(!(e->flags&F_CR2))memcpy(e->jpgp,"IMG_",4);else memcpy(e->jpgp,e->cr2p,4);
            frame_path(e,0,q,sizeof(q));
            if(FIO_RenameFile(tq,q)==0){e->flags|=F_JPG;e->done=1;
                {char d_[224];snprintf(d_,sizeof(d_),"recovered %s -> %s",tq,q);diag(d_);diag_flush();}}
        }else FIO_RemoveFile(tq);
    }
    for(i=j=0;i<nframes;i++)if(frames[i].flags&(F_CR2|F_JPG))frames[j++]=frames[i];
    nframes=j;
    /* Developed in place: the frame's own JPG carries our tag. Only the
     * newest such frame matters (ALL NEW starts after it), so read back
     * from the newest frame until one is found or a frame is already known
     * done -- at most 200 headers of 1 KB (a job takes 200 frames). */
    if(buf){
        int k=0;
        for(i=nframes-1;i>=0&&k<200;i--){
            frame_ent *e=&frames[i];
            if(e->done)break;
            if(!(e->flags&F_JPG)||(e->flags&F_DUP))continue;
            frame_path(e,0,p,sizeof(p));k++;
            if(soup_marked(p,buf,1024)){e->done=1;break;}
        }
        fio_free(buf);
    }
    return 0;
}
static void frame_path(const frame_ent *e,int cr2,char *out,int cap)
{
    /* No %c: Canon's firmware snprintf mangles it (repeats the first char
     * to the end of the buffer). Build the prefix as a string instead. */
    char p[5];
    memcpy(p,cr2?e->cr2p:e->jpgp,4);p[4]=0;
    snprintf(out,cap,"%s/%s%04d.%s",frames_dir,p,e->num,cr2?"CR2":"JPG");
}
/* The camera's last shot if it is in this folder, else the highest frame. */
static int latest_index(void)
{
    struct card_info *card=get_shooting_card();
    int at=card?frame_lookup(card->file_number):-1;
    if(at>=0)return at;
    return nframes-1;
}
/* Frames shot after the newest one Soup has developed in this folder. If
 * nothing here has been developed yet, "new" is just the latest frame:
 * a first use must never queue the whole card. */
static int select_new(uint16_t *out,int cap)
{
    int i,last=-1,n=0;
    for(i=0;i<nframes;i++)if(frames[i].done)last=i;
    if(last<0){i=latest_index();if(i>=0&&cap>0){out[0]=frames[i].num;return 1;}return 0;}
    for(i=last+1;i<nframes&&n<cap;i++)if(!frames[i].done)out[n++]=frames[i].num;
    return n;
}
static int select_last(uint16_t *out,int cap,int count)
{
    int i,n=0,start;
    if(count>cap)count=cap;
    start=nframes-count;if(start<0)start=0;
    for(i=start;i<nframes;i++)out[n++]=frames[i].num;
    return n;
}
/* ============================================================== EXIF ====
 * A small, bounds-checked TIFF reader for what Soup needs from either a CR2
 * (TIFF at offset 0) or a camera JPG (TIFF inside the Exif APP1 segment):
 * the 160x120 thumbnail, orientation, and the exposure data to carry into
 * the output. Anything missing or malformed simply stays zero.
 */
typedef struct {
    uint32_t thumb_off,thumb_len;     /* absolute within the buffer */
    int orient,iso;
    float exposure,fnumber;
    char make[32],model[48],date[20];
} meta_info;
static uint32_t rd16(const uint8_t *p,int le){return le?(uint32_t)(p[0]|p[1]<<8):(uint32_t)(p[0]<<8|p[1]);}
static uint32_t rd32(const uint8_t *p,int le){return le?(uint32_t)(p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24):((uint32_t)p[0]<<24|p[1]<<16|p[2]<<8|p[3]);}
static void tiff_ascii(const uint8_t *b,uint32_t len,uint32_t base,const uint8_t *ent,int le,char *dst,int cap)
{
    uint32_t count=rd32(ent+4,le),src,i;
    const uint8_t *p;
    if(count<=4)p=ent+8;
    else{src=rd32(ent+8,le);if(src>len||base>len-src||count>len-base-src)return;p=b+base+src;}
    for(i=0;i<count&&(int)i<cap-1&&p[i];i++)dst[i]=(char)p[i];
    dst[i]=0;
}
static float tiff_rational(const uint8_t *b,uint32_t len,uint32_t base,const uint8_t *ent,int le)
{
    uint32_t src=rd32(ent+8,le),num,den;
    if(src>len||base>len-src||8>len-base-src)return 0;
    num=rd32(b+base+src,le);den=rd32(b+base+src+4,le);
    return den?(float)num/(float)den:0;
}
/* which: 0 = IFD0, 1 = IFD1 (thumbnail), 2 = Exif IFD */
static uint32_t tiff_ifd(const uint8_t *b,uint32_t len,uint32_t base,int le,uint32_t off,int which,meta_info *m,uint32_t *exif)
{
    uint32_t n,i,t;const uint8_t *e;
    if(!off||off>len||base>len-off||2>len-base-off)return 0;
    n=rd16(b+base+off,le);
    if(n>256||2+12*n+4>len-base-off)return 0;
    for(i=0;i<n;i++){
        e=b+base+off+2+12*i;t=rd16(e,le);
        if(which==0){
            if(t==0x010F)tiff_ascii(b,len,base,e,le,m->make,sizeof(m->make));
            else if(t==0x0110)tiff_ascii(b,len,base,e,le,m->model,sizeof(m->model));
            else if(t==0x0132&&!m->date[0])tiff_ascii(b,len,base,e,le,m->date,sizeof(m->date));
            else if(t==0x0112)m->orient=(int)rd16(e+8,le);
            else if(t==0x8769)*exif=rd32(e+8,le);
        }else if(which==1){
            if(t==0x0201)m->thumb_off=rd32(e+8,le);
            else if(t==0x0202)m->thumb_len=rd32(e+8,le);
        }else{
            if(t==0x829A)m->exposure=tiff_rational(b,len,base,e,le);
            else if(t==0x829D)m->fnumber=tiff_rational(b,len,base,e,le);
            else if(t==0x8827)m->iso=(int)rd16(e+8,le);
            else if(t==0x9003)tiff_ascii(b,len,base,e,le,m->date,sizeof(m->date)); /* DateTimeOriginal wins */
        }
    }
    return rd32(b+base+off+2+12*n,le);
}
static void tiff_meta(const uint8_t *b,uint32_t len,uint32_t base,meta_info *m)
{
    int le;uint32_t ifd0,ifd1,exif=0;
    if(base>len||8>len-base)return;
    if(b[base]=='I'&&b[base+1]=='I')le=1;else if(b[base]=='M'&&b[base+1]=='M')le=0;else return;
    ifd0=rd32(b+base+4,le);
    ifd1=tiff_ifd(b,len,base,le,ifd0,0,m,&exif);
    if(ifd1)tiff_ifd(b,len,base,le,ifd1,1,m,&exif);
    if(exif)tiff_ifd(b,len,base,le,exif,2,m,&exif);
    /* thumbnail offsets are TIFF-relative; make them absolute and keep them
     * only if the whole thumbnail is inside what was read */
    if(m->thumb_len&&m->thumb_off<=len&&base<=len-m->thumb_off&&m->thumb_len<=len-base-m->thumb_off)
        m->thumb_off+=base;
    else m->thumb_off=m->thumb_len=0;
    if(m->orient<1||m->orient>8)m->orient=1;
}
/* is_cr2: TIFF at 0. Otherwise find the Exif APP1 segment of a JPEG. */
static void read_meta(const uint8_t *b,uint32_t len,int is_cr2,meta_info *m)
{
    uint32_t p=2,seg;
    memset(m,0,sizeof(*m));m->orient=1;
    if(is_cr2){tiff_meta(b,len,0,m);return;}
    if(len<4||b[0]!=0xFF||b[1]!=0xD8)return;
    while(p+4<=len&&b[p]==0xFF){
        uint8_t mk=b[p+1];
        if(mk==0xDA||mk==0xD9)break;
        seg=rd16(b+p+2,0);
        if(seg<2)break;
        if(mk==0xE1&&seg>=16&&p+10<=len&&!memcmp(b+p+4,"Exif\0\0",6)){tiff_meta(b,len,p+10,m);return;}
        p+=2+seg;
    }
}

/* ========================================================= thumbnails ===
 * Decode a small embedded JPEG to 8-bit luma for the picker. */
#define THUMB_W 160
#define THUMB_H 120
typedef struct {uint8_t *gray;int w,h,rows;} thumb_ctx;
static int thumb_sink(void *ctx,const uint8_t *rgb,int nrows,int width)
{
    thumb_ctx *t=ctx;int r,x;
    if(width>THUMB_W)return -1;
    t->w=width;
    for(r=0;r<nrows&&t->rows<THUMB_H;r++,t->rows++)
        for(x=0;x<width;x++){
            const uint8_t *p=rgb+((size_t)r*width+x)*3;
            t->gray[t->rows*THUMB_W+x]=(uint8_t)((77*p[0]+150*p[1]+29*p[2])>>8);
        }
    t->h=t->rows;
    return 0;
}
static int decode_thumb_gray(const uint8_t *jpg,uint32_t len,void *arena_mem,size_t arena_cap,uint8_t *gray,int *w,int *h)
{
    se_arena a;se_jdec_info ji;thumb_ctx t;
    memset(&t,0,sizeof(t));t.gray=gray;
    se_arena_init(&a,arena_mem,arena_cap);
    if(se_jpeg_decode_scaled(jpg,len,1,&a,thumb_sink,&t,&ji)!=SE_OK||t.w<8||t.h<8)return -1;
    *w=t.w;*h=t.h;return 0;
}
/* ================================================================ jobs ===
 * A job is a list of frame numbers developed one after another with one
 * stock and the settings as they were when it started. Per frame:
 *   source  Canon's JPG when it is exactly the output size (decoded 1:1 --
 *           RAW+S at HALF: a quarter of the decode work) or twice it
 *           (decoded at half), otherwise the CR2's embedded preview. The
 *           output size is the frame's largest picture, halved for HALF.
 *           Both are read-only.
 *   output  ONE JPEG (default) / IN PLACE: the frame's own JPG name, which
 *           Canon playback already lists. Written as SOUI####.TMP, verified,
 *           then Canon's JPG removed and the TMP renamed over it; ONE JPEG
 *           then removes the CR2 too. A JPG we developed is never a source:
 *           only the CR2 can be developed again.
 *           NEW FILE: SOUP#### with the next free number, in the shooting
 *           folder or ML/SOUP; never an overwrite.
 *   delete  NEW FILE + DELETE RAW: the CR2 only, only when developed from a
 *           JPG of the same frame at least as large as the CR2 preview, only after the output re-reads complete.
 */
#define MAX_JOB 200
static uint16_t job_nums[MAX_JOB];
static int job_total;
static volatile int job_pos;
static int job_ok, job_failed, job_skipped, job_deleted, job_kept;
static int job_out;                    /* NEW FILE modes: 0 camera folder, 1 ML/SOUP */
static int auto_base=-1, auto_upto;    /* auto develop: frames after auto_base, up to auto_upto */
static int lv_ago=99;                  /* seconds since Live View was last on */
static const char *const mode_names[M_COUNT]={"JPEG ONLY","REPLACE JPEG","NEW JPEG","NEW JPEG + DELETE RAW","ML/SOUP FOLDER"};
static char last_out[96], last_from[4];
static char job_log[2048]; static int job_log_len;

static void log_line(const char *s)
{
    int n=strlen(s);
    if(job_log_len+n+2>=(int)sizeof(job_log))return;
    memcpy(job_log+job_log_len,s,n);job_log_len+=n;
    job_log[job_log_len++]='\r';job_log[job_log_len++]='\n';job_log[job_log_len]=0;
}

typedef struct {int is_cr2;uint32_t size,offset,length;size_t hdr_len;se_jdec_info ji;} source_info;
/* Read the start of a CR2 or JPG into `header` and locate the JPEG stream:
 * the embedded full-size preview for a CR2, the whole file for a JPG. */
static void probe_log(const char *path,const char *step,int v)
{
    char t[128];snprintf(t,sizeof(t),"probe %s: %s (%d)",path,step,v);diag(t);log_line(t);
}
static int probe_source(const char *path,int is_cr2,void *header,source_info *s,uint32_t *got)
{
    FILE *f;uint32_t n;int pw,ph,rc;
    memset(s,0,sizeof(*s));s->is_cr2=is_cr2;*got=0;
    if((rc=FIO_GetFileSize(path,&s->size))!=0){probe_log(path,"getsize rc",rc);return -1;}
    if(s->size<64){probe_log(path,"too small",(int)s->size);return -1;}
    if(is_cr2&&s->size<HDR_CAP){probe_log(path,"cr2 too small",(int)s->size);return -1;}
    n=s->size<HDR_CAP?s->size:HDR_CAP;
    f=FIO_OpenFile(path,O_RDONLY);if(!good_file(f)){probe_log(path,"open failed",(int)f);return -1;}
    if(read_exact(f,0,header,n)!=0){FIO_CloseFile(f);probe_log(path,"read failed n",(int)n);return -1;}
    FIO_CloseFile(f);*got=n;
    if(is_cr2){if((rc=se_cr2_find_preview(header,n,&s->offset,&s->length,&pw,&ph))!=SE_OK){probe_log(path,"cr2 preview rc",rc);return -1;}}
    else{s->offset=0;s->length=s->size;}
    if(!s->length||s->length>MAX_PREVIEW||s->offset>=n||s->offset>s->size||s->length>s->size-s->offset){probe_log(path,"bad bounds len",(int)s->length);return -1;}
    s->hdr_len=n-s->offset;if(s->hdr_len>s->length)s->hdr_len=s->length;
    if((rc=se_jpeg_dec_info((uint8_t*)header+s->offset,s->hdr_len,&s->ji))!=SE_OK){probe_log(path,"dec_info rc",rc);return -1;}
    if(s->ji.width<32||s->ji.width>6000||s->ji.height<16||s->ji.height>4500){probe_log(path,"bad dims w",s->ji.width);return -1;}
    return 0;
}
/* Re-read a finished output: exact size, SOI first, EOI last. */
static int verify_jpeg(const char *path,uint32_t size)
{
    FILE *f;uint8_t *b;uint32_t got=0;int ok=-1;
    if(size<4||FIO_GetFileSize(path,&got)!=0||got!=size)return -1;
    b=fio_malloc(64);if(!b)return -1;
    f=FIO_OpenFile(path,O_RDONLY);
    if(good_file(f)){
        if(read_exact(f,0,b,2)==0&&b[0]==0xFF&&b[1]==0xD8&&
           read_exact(f,size-2,b+2,2)==0&&b[2]==0xFF&&b[3]==0xD9)ok=0;
        FIO_CloseFile(f);
    }
    fio_free(b);return ok;
}

/* The output's EXIF thumbnail: the source's own 160x120 thumbnail run
 * through the same look and re-encoded small, so Canon's index view shows
 * the graded frame rather than the original. Optional: any failure just
 * means no thumbnail. Uses its own short-lived allocation, freed before
 * the main develop allocates. */
#define TH_CAP 7000                    /* must fit the encoder's 8 KB EXIF block */
typedef struct {uint8_t *buf;size_t len,cap;} membuf;
static int mem_sink(void *ctx,const uint8_t *d,size_t n)
{
    membuf *m=ctx;
    if(n>m->cap-m->len)return -1;
    memcpy(m->buf+m->len,d,n);m->len+=n;return 0;
}
typedef struct {se_jpeg *enc;se_look_run *look;int width;} pipeline;
static int th_emit(void *ctx,const uint8_t *row,int width)
{
    pipeline *p=ctx;
    if(width!=p->width)return -1;
    return se_jpeg_write_rows(p->enc,row,1)==SE_OK?0:-1;
}
static int th_rows(void *ctx,const uint8_t *rgb,int rows,int width)
{
    pipeline *p=ctx;
    if(JOB_STOP()||width!=p->width)return -1;
    return se_look_rows(p->look,rgb,rows,th_emit,p)==SE_OK?0:-1;
}
static int graded_thumb(const se_stock *stock,const uint8_t *jpg,uint32_t len,uint8_t *out,int src_width)
{
    se_arena a,la;se_jdec_info ji;se_look lk;pipeline p;membuf mb;uint8_t *mem,*strip;
    size_t ls;int result=0;
    if(!jpg||len<64)return 0;
    mem=tmp_malloc(160*1024);if(!mem)return 0;
    memset(&p,0,sizeof(p));
    se_arena_init(&a,mem,96*1024);
    if(se_jpeg_dec_info(jpg,len,&ji)!=SE_OK||ji.width>THUMB_W||ji.width<16)goto out;
    /* grain and halation are scaled to the thumbnail's size, not the frame's */
    (void)src_width;
    se_look_from_stock(stock,4,&lk);   /* coarsest supported scale: grain and glow stay small */
    ls=se_look_fit(&lk,ji.width,60*1024);
    se_arena_init(&la,mem+96*1024,ls<64*1024?ls:64*1024);
    p.enc=SE_NEW(&a,se_jpeg,1);strip=SE_NEW(&a,uint8_t,(size_t)8*ji.width*3);p.width=ji.width;
    if(!p.enc||!strip||se_look_begin(&p.look,&lk,ji.width,&la)!=SE_OK)goto out;
    mb.buf=out;mb.len=0;mb.cap=TH_CAP;
    if(se_jpeg_begin(p.enc,ji.width,ji.height,75,strip,0,mem_sink,&mb)!=SE_OK)goto out;
    if(se_jpeg_decode_scaled(jpg,len,1,&a,th_rows,&p,&ji)!=SE_OK)goto out;
    if(se_look_end(p.look,th_emit,&p)!=SE_OK||se_jpeg_end(p.enc)!=SE_OK)goto out;
    result=(int)mb.len;
out:
    free(mem);return result;
}

typedef struct {FILE *f;uint32_t base,total;void *dma;} reader;
static int preview_read(void *ctx,uint32_t off,void *dst,uint32_t n)
{
    reader *r=ctx;
    if(JOB_STOP()||n>IO_CAP||off>r->total||n>r->total-off)return -1;
    if(read_exact(r->f,r->base+off,r->dma,n)!=0)return -1;
    progress_pct=(int)((uint64_t)(off+n)*100u/r->total);
    memcpy(dst,r->dma,n);return 0;
}
typedef struct {FILE *f;void *dma;uint32_t used,total;int error;} writer;
static int flush_output(writer *w)
{
    uint32_t done=0;
    while(done<w->used){
        int n=FIO_WriteFile(w->f,(uint8_t*)w->dma+done,w->used-done);
        if(n<=0||(uint32_t)n>w->used-done){w->error=1;return -1;}
        done+=(uint32_t)n;
    }
    w->total+=w->used;w->used=0;return 0;
}
static int jpeg_sink(void *ctx,const uint8_t *data,size_t len)
{
    writer *w=ctx;
    if(JOB_STOP()||w->error)return -1;
    while(len){
        size_t n=IO_CAP-w->used;if(n>len)n=len;
        memcpy((uint8_t*)w->dma+w->used,data,n);
        w->used+=n;data+=n;len-=n;
        if(w->used==IO_CAP&&flush_output(w)!=0)return -1;
    }
    return 0;
}
static int emit_row(void *ctx,const uint8_t *row,int width)
{
    pipeline *p=ctx;
    if(JOB_STOP()||width!=p->width)return -1;
    return se_jpeg_write_rows(p->enc,row,1)==SE_OK?0:-1;
}
static int grade_rows(void *ctx,const uint8_t *rgb,int rows,int width)
{
    pipeline *p=ctx;
    static int last_yield;
    int t0=get_ms_clock();
    if(JOB_STOP()||width!=p->width)return -1;
    /* Yield so Canon's lower-priority tasks keep running (the GUI task
     * outranks this one and preempts it anyway). msleep(1) costs a whole
     * 10 ms DryOS tick: once per MCU row was 2.3 s a frame, so only after
     * ~500 ms of work. */
    if(t0-last_yield>=500){msleep(1);last_yield=get_ms_clock();}
    return se_look_rows(p->look,rgb,rows,emit_row,p)==SE_OK?0:-1;
}

/* Our EXIF Software tag sits in the first few hundred bytes of a JPG. */
static int has_tag(const void *h,uint32_t n)
{
    const uint8_t *b=h;uint32_t i;
    if(n>4096)n=4096;
    for(i=0;i+11<=n;i++)if(b[i]=='S'&&!memcmp(b+i,"SOUP ENGINE",11))return 1;
    return 0;
}

/* Develop one frame. 1 ok, 0 skipped, -1 failed. */
static int develop_frame(int num,const se_stock *stock,char *text_scratch)
{
    FILE *in=0,*out=0;void *header=0,*memory=0,*read_dma=0,*write_dma=0;
    uint32_t check_size=0,got=0;
    int at,ow=0,oh=0,success=0,created=0,have_cr2,scale=job_scale,raw_deleted=0,thumb_len=0,version=0;
    int inplace=job_mode<=M_INPLACE,jpg_soup=0,skipped=0,keep_temp=0;
    se_err rc=SE_E_IO;se_look look;se_jpeg_exif ex;meta_info meta;
    se_arena arena,look_arena;pipeline pipe;source_info src,alt;
    reader rd;writer wr;size_t look_size;uint8_t *strip,*thumb=0;
    frame_ent *e;char cr2_path[96],jpg_path[96],input_path[96],output_path[96],temp_path[96],sw[64];
    const char *why="failed";
    int t_start=get_ms_clock();
    memset(&pipe,0,sizeof(pipe));memset(&wr,0,sizeof(wr));memset(&src,0,sizeof(src));memset(&ex,0,sizeof(ex));
    memset(&meta,0,sizeof(meta));input_path[0]=0;
    at=frame_lookup(num);
    if(at<0){snprintf(text_scratch,128,"%04d not found",num);log_line(text_scratch);return 0;}
    e=&frames[at];
    if(e->flags&F_DUP){snprintf(text_scratch,128,"%04d skipped: several files with this number",num);log_line(text_scratch);return 0;}
    /* a RAW-only frame gets its JPG under the CR2's prefix */
    if(!(e->flags&F_JPG))memcpy(e->jpgp,e->cr2p,4);
    frame_path(e,1,cr2_path,sizeof(cr2_path));frame_path(e,0,jpg_path,sizeof(jpg_path));
    if(inplace){
        snprintf(output_path,sizeof(output_path),"%s",jpg_path);
        inplace_tmp(num,temp_path,sizeof(temp_path));
        if(is_file(temp_path))FIO_RemoveFile(temp_path);   /* ours, from a failed try */
    }else if((version=next_output(job_out,output_path,temp_path,sizeof(output_path)))<0){
        snprintf(text_scratch,128,"%04d skipped: no free file number (9999 reached)",num);log_line(text_scratch);return 0;}
    header=fio_malloc(HDR_CAP);if(!header){why="no mem header";goto done;}
    have_cr2=(e->flags&F_CR2)&&probe_source(cr2_path,1,header,&alt,&got)==0;
    if((e->flags&F_JPG)&&probe_source(jpg_path,0,header,&src,&got)==0&&!(jpg_soup=has_tag(header,got))){
        int fw=src.ji.width,fh=src.ji.height,tw,th,w2,h2;
        if(have_cr2&&alt.ji.width>fw){fw=alt.ji.width;fh=alt.ji.height;}
        se_jpeg_scaled_size(fw,fh,job_scale,&tw,&th);
        se_jpeg_scaled_size(src.ji.width,src.ji.height,2,&w2,&h2);
        scale=(src.ji.width==tw&&src.ji.height==th)?1:(w2==tw&&h2==th)?2:0;
        if(scale)snprintf(input_path,sizeof(input_path),"%s",jpg_path);
    }
    if(!input_path[0]){
        scale=job_scale;
        /* never grade our own output a second time */
        if(jpg_soup&&!have_cr2){why="already developed";skipped=1;goto done;}
        if(have_cr2&&probe_source(cr2_path,1,header,&src,&got)==0)
            snprintf(input_path,sizeof(input_path),"%s",cr2_path);
        else{why="unreadable";goto done;}
    }
    read_meta(header,got,src.is_cr2,&meta);
    /* graded EXIF thumbnail, from the header already in memory */
    thumb=malloc(TH_CAP);
    if(thumb&&meta.thumb_len)thumb_len=graded_thumb(stock,(uint8_t*)header+meta.thumb_off,meta.thumb_len,thumb,src.ji.width);
    snprintf(sw,sizeof(sw),"SOUP ENGINE - %s #%04d",stock_names[job_stock],num);
    ex.make=meta.make[0]?meta.make:"Canon";ex.model=meta.model[0]?meta.model:"Canon EOS 7D";
    ex.software=sw;ex.datetime=meta.date[0]?meta.date:0;
    ex.exposure_time=meta.exposure;ex.aperture=meta.fnumber;ex.iso=meta.iso;ex.orientation=meta.orient;
    ex.thumb=thumb_len>0?thumb:0;ex.thumb_len=thumb_len;

    in=FIO_OpenFile(input_path,O_RDONLY);if(!good_file(in)){in=0;goto done;}
    se_jpeg_scaled_size(src.ji.width,src.ji.height,scale,&ow,&oh);
    /* grain and glow are sized against the full frame, not the source */
    se_look_from_stock(stock,job_scale,&look);
    look_size=se_look_fit(&look,ow,SE_LOOK_BUDGET);
    memory=tmp_malloc(SCRATCH_CAP+look_size+64);read_dma=fio_malloc(IO_CAP);write_dma=fio_malloc(IO_CAP);
    if(!memory||!read_dma||!write_dma){why=!memory?"no mem scratch":!read_dma?"no mem read_dma":"no mem write_dma";goto done;}
    se_arena_init(&arena,memory,SCRATCH_CAP);
    se_arena_init(&look_arena,(uint8_t*)memory+SCRATCH_CAP,look_size);
    pipe.enc=SE_NEW(&arena,se_jpeg,1);pipe.width=ow;
    strip=SE_NEW(&arena,uint8_t,(size_t)8*ow*3);
    if(!pipe.enc||!strip)goto done;
    if(se_look_begin(&pipe.look,&look,ow,&look_arena)!=SE_OK)goto done;
    if((!inplace&&is_file(output_path))||is_file(temp_path))goto done;
    /* O_EXCL is not assumed supported by Canon's FIO; names are chosen
     * unused and rechecked immediately, and only this task writes them. */
    out=FIO_CreateFile(temp_path);if(!good_file(out)){out=0;why="cannot create output";goto done;}created=1;
    wr.f=out;wr.dma=write_dma;
    if(se_jpeg_begin(pipe.enc,ow,oh,88,strip,&ex,jpeg_sink,&wr)!=SE_OK)goto done;
    rd.f=in;rd.base=src.offset;rd.total=src.length;rd.dma=read_dma;
    rc=se_jpeg_decode_stream_scaled((uint8_t*)header+src.offset,src.hdr_len,src.length,
        preview_read,&rd,scale,&arena,grade_rows,&pipe,&src.ji);
    if(rc!=SE_OK||se_look_end(pipe.look,emit_row,&pipe)!=SE_OK)goto done;
    if(se_jpeg_end(pipe.enc)!=SE_OK||flush_output(&wr)!=0||wr.error)goto done;
    FIO_CloseFile(out);out=0;
    FIO_CloseFile(in);in=0;            /* the source may be the file being replaced */
    if(FIO_GetFileSize(temp_path,&check_size)!=0||check_size!=wr.total||!check_size)goto done;
    if(inplace){
        if(verify_jpeg(temp_path,wr.total)!=0){why="verify failed";goto done;}
        if(JOB_STOP())goto done;
        if(is_file(output_path)&&(FIO_RemoveFile(output_path)!=0||is_file(output_path))){why="cannot remove camera jpg";goto done;}
        /* From here the TMP is the frame's only picture: it is never removed
         * on failure, and the next scan finishes the rename. */
        keep_temp=1;
        if(FIO_RenameFile(temp_path,output_path)!=0){why="rename failed, SOUI tmp kept";goto done;}
        keep_temp=0;
        if(FIO_GetFileSize(output_path,&check_size)!=0||check_size!=wr.total){why="size check after rename";goto done;}
        success=1;
        e->flags|=F_JPG;
        if(job_mode==M_ONE&&(e->flags&F_CR2)){
            if(FIO_RemoveFile(cr2_path)==0&&!is_file(cr2_path)){raw_deleted=1;job_deleted++;e->flags&=~F_CR2;}
            else job_kept++;
        }
        goto done;
    }
    if(is_file(output_path))goto done;
    if(FIO_RenameFile(temp_path,output_path)!=0){why="rename failed";goto done;}
    if(FIO_GetFileSize(output_path,&check_size)!=0||check_size!=wr.total)goto done;
    success=1;
    if(job_delete&&have_cr2&&!JOB_STOP()){
        /* no full-size JPG (none, or RAW+S): the CR2 is the original */
        if(src.is_cr2||!have_cr2||src.ji.width<alt.ji.width)job_kept++;
        else if(verify_jpeg(output_path,wr.total)==0&&is_file(jpg_path)&&
                FIO_RemoveFile(cr2_path)==0&&!is_file(cr2_path)){raw_deleted=1;job_deleted++;}
        else job_kept++;
    }
done:
    if(out)FIO_CloseFile(out);
    if(in)FIO_CloseFile(in);
    if(!success&&created&&!keep_temp&&is_file(temp_path))FIO_RemoveFile(temp_path);
    if(header)fio_free(header);
    if(read_dma)fio_free(read_dma);
    if(write_dma)fio_free(write_dma);
    if(memory)free(memory);
    if(thumb)free(thumb);
    if(success){
        snprintf(last_out,sizeof(last_out),"%s",output_path);
        snprintf(last_from,sizeof(last_from),"%s",src.is_cr2?"CR2":"JPG");
        e->done=1;(void)version;
    }
    snprintf(text_scratch,256,"%04d %s from=%s out=%s scale=%d thumb=%d orient=%d raw_deleted=%d ms=%d",
        num,success?"ok":skipped?"skipped: already developed (no CR2 left)":JOB_STOP()?"cancelled":why,input_path[0]?input_path:"-",
        success?output_path:"-",scale,thumb_len,meta.orient,raw_deleted,get_ms_clock()-t_start);
    log_line(text_scratch);
    return success?1:skipped?0:-1;
}

static void job_settings(void)
{
    job_scale=set_res==0?1:2;job_mode=set_mode;
    job_delete=set_mode==M_NEWDEL;job_out=set_mode==M_MLSOUP;
}

/* Automatic develop: everything shot after auto_base, up to auto_upto, that
 * is not developed yet. Runs in the job task, so the clock callback that
 * starts it never touches the card. 0 = nothing to do. */
static int auto_select(void)
{
    int i,n=0;
    ensure_loaded();
    if(!set_auto||!stock_count)return 0;
    job_stock=selected_stock;if(job_stock<0||job_stock>=stock_count)return 0;
    job_settings();
    if(index_build()!=0)return 0;
    for(i=0;i<nframes&&n<MAX_JOB;i++){
        frame_ent *e=&frames[i];
        if(e->num>auto_base&&e->num<=auto_upto&&!e->done&&!(e->flags&F_DUP))job_nums[n++]=e->num;
    }
    return n;
}

static void job_task(void *unused)
{
    char text[16384],line[256];se_stock stock;int n,i,r,stopped=0;
    (void)unused;
    job_log_len=0;job_log[0]=0;
    if(job_auto){
        job_total=auto_select();
        if(!job_total){auto_base=auto_upto;if(!gui_task_alive)frames_release();job_auto=0;busy=0;return;}
        /* Show the developing screen, as if Soup had been opened by hand: it
         * closes itself when done, or at once on a half-press. Not over Live
         * View, playback or a menu -- there the end-of-job note is enough.
         * lv reads 0 while a Live View shot is captured and reviewed, so
         * Live View in the last 10 s counts as Live View. */
        if(!lv&&lv_ago>=10&&!gui_task_alive&&!gui_menu_shown()&&!is_pure_play_photo_or_movie_mode()){
            gui_auto=1;if(gui_open()!=0)gui_auto=0;
        }
    }
    powersave_prohibit();                 /* no auto power-off mid-develop */
    snprintf(line,sizeof(line),"folder=%s stock=%s frames=%d scale=%d mode=%s%s",
        frames_dir,stock_names[job_stock],job_total,job_scale,mode_names[job_mode],job_auto?" auto":"");
    log_line(line);
    snprintf(line,sizeof(line),"mem malloc_free=%d am_free=%d",GetFreeMemForMalloc(),GetFreeMemForAllocateMemory());
    log_line(line);
    n=read_text(stock_paths[job_stock],text,sizeof(text));
    se_stock_init(&stock);
    if(n<=0||se_stock_parse(text,(size_t)n,&stock)!=SE_OK){
        snprintf(line,sizeof(line),"stock unreadable: %s rc=%d",stock_paths[job_stock],n);
        log_line(line);job_failed=job_total;if(job_auto)auto_base=auto_upto;goto out;}
    for(i=0;i<job_total;i++){
        if(JOB_STOP()){stopped=1;break;}
        job_pos=i;progress_pct=0;
        snprintf(status_text,sizeof(status_text),"Developing %d of %d",i+1,job_total);
        r=develop_frame(job_nums[i],&stock,line);
        if(r<0&&JOB_STOP()){stopped=1;break;}
        if(r>0)job_ok++;else if(r==0)job_skipped++;else job_failed++;
        /* auto: anything finished, skipped or failed is behind us (failures
         * are in FIND.TXT); a frame that was stopped is tried again */
        if(job_auto)auto_base=job_nums[i];
    }
    if(job_auto&&!stopped)auto_base=auto_upto;
out:
    write_small_file(FIND_LOG,job_log);
    diag_flush();
    powersave_permit();
    job_ms=get_ms_clock()-job_t0;
    job_result=job_ok?1:(cancel_job?-1:job_skipped&&!job_failed?-2:-1);
    snprintf(status_text,sizeof(status_text),"%d saved, %d failed",job_ok,job_failed);
    if(!ml_shutdown_requested&&!stop_worker&&!gui_running){
        if(!job_auto)NotifyBox(5000,"Soup: %d saved, %d failed, %d skipped",job_ok,job_failed,job_skipped);
        else if(job_ok||job_failed)NotifyBox(2000,"Soup: %d developed%s",job_ok,job_failed?", some failed":"");
    }
    job_auto=0;
    if(!gui_task_alive)frames_release();   /* GUI gone: nobody needs the index */
    busy=0;
}

/* Start developing `count` frames (index must be current). NULL = started. */
static const char *begin_job(const uint16_t *nums,int count)
{
    int i;
    if(busy||stop_worker||ml_shutdown_requested)return "BUSY";
    if(!stock_count)return "NO FILM STOCKS";
    if(count<1)return "NO PHOTOS TO DEVELOP";
    if(count>MAX_JOB)count=MAX_JOB;
    if(!is_dir("ML/SOUP"))FIO_CreateDirectory("ML/SOUP");
    if(!is_dir("ML/SOUP"))return "CANNOT CREATE ML/SOUP";
    job_stock=selected_stock;if(job_stock<0||job_stock>=stock_count)return "NO STOCK SELECTED";
    for(i=0;i<count;i++)job_nums[i]=nums[i];
    job_total=count;job_pos=0;job_ok=job_failed=job_skipped=job_deleted=job_kept=0;
    last_out[0]=0;last_from[0]=0;
    job_settings();
    cancel_job=0;progress_pct=0;job_result=0;job_t0=get_ms_clock();
    job_auto=0;busy=1;
    if(IS_ERROR(task_create("soup7d",0x1f,0x10000,job_task,0))){busy=0;return "TASK CREATION FAILED";}
    return 0;
}

/*
 * Auto develop, checked once a second from ML's clock task. Cheap and no
 * card I/O: it only watches Canon's file counter. Once a new shot has been
 * followed by two quiet seconds with the camera idle (capture job finished,
 * shutter not half-pressed, Soup screen closed), the job task is started in
 * auto mode and works out what to develop itself. The first shot after boot
 * also loads the settings; with AUTO off that is all that happens.
 */
static int auto_seen=-1, auto_quiet;
static unsigned int soup_clock(unsigned int ctx)
{
    struct card_info *card=get_shooting_card();
    int fn;
    (void)ctx;
    if(lv)lv_ago=0;else if(lv_ago<99)lv_ago++;
    if(!card||stop_worker||ml_shutdown_requested)return CBR_RET_CONTINUE;
    fn=card->file_number;
    if(auto_base<0){auto_base=auto_seen=fn;return CBR_RET_CONTINUE;}   /* boot: start from here */
    if(fn!=auto_seen){
        if(fn<auto_seen)auto_base=fn-1;   /* numbering restarted (new folder, wiped card) */
        auto_seen=fn;auto_quiet=0;return CBR_RET_CONTINUE;
    }
    if(fn<=auto_base||busy||gui_task_alive)return CBR_RET_CONTINUE;
    if(loaded&&!set_auto){auto_base=fn;return CBR_RET_CONTINUE;}
    /* not while reviewing pictures or in ML's menu: a file must not be
     * swapped under Canon's playback while it is on screen */
    if(++auto_quiet<2||lens_info.job_state||get_halfshutter_pressed()||
       gui_menu_shown()||is_pure_play_photo_or_movie_mode())return CBR_RET_CONTINUE;
    auto_upto=fn;job_total=0;job_pos=0;job_ok=job_failed=job_skipped=job_deleted=job_kept=0;
    last_out[0]=0;last_from[0]=0;
    cancel_job=0;progress_pct=0;job_result=0;job_t0=get_ms_clock();
    job_auto=1;busy=1;
    if(IS_ERROR(task_create("soup7d",0x1f,0x10000,job_task,0))){job_auto=0;busy=0;}
    return CBR_RET_CONTINUE;
}
/* ===================================================== Soup screen ====
 * Picture Style (outside Live View, playback and ML's menu) opens a
 * full-screen Soup view. Same mechanism as ML's own arkanoid module: open
 * ML's menu so Canon's GUI is suspended, block ML's menu redraw, and paint
 * the whole overlay from our own task. Buttons arrive through the standard
 * module CBR_KEYPRESS hook. No ROM addresses, palette registers or GUI-task
 * patches -- unlike the 5D payload, which needs all three.
 *
 * The engine UI renders a 720x240 4-bit buffer (the 5D's native overlay);
 * the 7D overlay is 720x480 8-bit, so rows are doubled and the 16 UI
 * colours are mapped onto ML's standard palette. Picker thumbnails are
 * drawn straight into the 8-bit overlay in ML's grey ramp: the palette has
 * no room for colour previews.
 */
enum {G_STOCK, G_SETTINGS, G_DEVMENU, G_PICK, G_WORK, G_DONE};
enum {REQ_NONE, REQ_DEVMENU, REQ_LATEST, REQ_NEW, REQ_LASTN, REQ_PICKONE};
static se_ui gui;
static se_ui_stock gui_stocks[MAX_STOCKS];
static volatile int gui_dirty, gui_close, gui_req, gui_save, gui_restock;
static volatile int gmode, gsetting, gdev, pick_i;
static char gui_note[64], gui_info[64], gui_v[4][24], gui_lasthelp[32], gui_right[24], gui_title[24];
static int dev_latest=-1, dev_new;
static const int lastn_choices[5]={2,3,5,10,20};
static uint16_t sel_buf[MAX_JOB];

/* picker state, owned by the GUI task */
static uint8_t *pk_hdr, *pk_arena, *pk_gray;
static int pk_loaded=-2, pk_ok, pk_w, pk_h, pk_orient;

static const uint8_t gui_colours[16]={
    COLOR_BLACK,        /* CLEAR  (opaque here: nothing should show through) */
    COLOR_GRAY(6),      /* INK    */
    COLOR_GRAY(22),     /* SHADOW */
    COLOR_GRAY(38),     /* SLATE  */
    COLOR_GRAY(55),     /* MID    */
    COLOR_GRAY(72),     /* EDGE   */
    COLOR_GRAY(88),     /* PAPER  */
    COLOR_GRAY(96),     /* HILITE */
    COLOR_DARK_RED,     /* RUST   */
    COLOR_RED,          /* RED    */
    COLOR_ORANGE,       /* AMBER  */
    COLOR_YELLOW,       /* GOLD   */
    COLOR_GREEN2,       /* OLIVE  */
    COLOR_CYAN,         /* TEAL   */
    COLOR_MAGENTA,      /* PLUM   */
    COLOR_WHITE,        /* WHITE  */
};

static void gui_blit(const uint8_t *fb)
{
    uint8_t *v=bmp_vram();int x,y;
    if(!v)return;
    for(y=0;y<SE_UI_H;y++){
        const uint8_t *src=fb+y*SE_UI_PITCH;
        uint8_t *d0=v+(2*y)*BMPPITCH,*d1=d0+BMPPITCH;
        for(x=0;x<SE_UI_PITCH;x++){
            uint8_t b=src[x],lo=gui_colours[b&15],hi=gui_colours[b>>4];
            /* even x in the low nibble, odd x in the high one (ui.c) */
            d0[2*x]=lo;d0[2*x+1]=hi;d1[2*x]=lo;d1[2*x+1]=hi;
        }
    }
}

/* Thumbnail into a box of the 8-bit overlay, rotated per EXIF and scaled
 * (nearest) to fit, in ML's 42-step grey ramp. */
#define PK_BX 200
#define PK_BY 68
#define PK_BW 320
#define PK_BH 240
static void pick_blit_thumb(const uint8_t *gray,int w,int h,int orient)
{
    uint8_t *v=bmp_vram();int dw,dh,ox,oy,dx,dy,sx,sy,num,den;
    if(!v||w<1||h<1)return;
    if(orient>=5){dw=h;dh=w;}else{dw=w;dh=h;}
    /* scale = num/den, fitting dw x dh into the box */
    if((long)PK_BW*dh<=(long)PK_BH*dw){num=PK_BW;den=dw;}else{num=PK_BH;den=dh;}
    ox=PK_BX+(PK_BW-dw*num/den)/2;oy=PK_BY+(PK_BH-dh*num/den)/2;
    for(dy=0;dy<dh*num/den;dy++){
        uint8_t *row=v+(oy+dy)*BMPPITCH+ox;
        int ry=dy*den/num;
        for(dx=0;dx<dw*num/den;dx++){
            int rx=dx*den/num;
            switch(orient){
            case 3: sx=w-1-rx;sy=h-1-ry;break;
            case 6: sx=ry;sy=h-1-rx;break;
            case 8: sx=w-1-ry;sy=rx;break;
            default:sx=rx;sy=ry;break;
            }
            row[dx]=(uint8_t)COLOR_GRAY(gray[sy*THUMB_W+sx]*100/255);
        }
    }
}

static void pick_free(void)
{
    if(pk_hdr){fio_free(pk_hdr);pk_hdr=0;}
    if(pk_arena){free(pk_arena);pk_arena=0;}
    if(pk_gray){free(pk_gray);pk_gray=0;}
    pk_loaded=-2;
}
/* Load the thumbnail of frames[i]. Prefers the JPG (EXIF is in its first
 * 64 KB); a CR2's thumbnail sits about 57 KB in on the 7D. */
static int pk_logged;
static void pick_load(int i)
{
    frame_ent *e;char path[96];FILE *f;uint32_t size=0,n;meta_info m;int jpg;
    pk_loaded=i;pk_ok=0;
    if(i<0||i>=nframes)return;
    if(!pk_hdr)pk_hdr=fio_malloc(HDR_CAP);
    if(!pk_arena)pk_arena=tmp_malloc(64*1024);
    if(!pk_gray)pk_gray=malloc(THUMB_W*THUMB_H);
    if(!pk_hdr||!pk_arena||!pk_gray){
        static int said;char t[96];
        if(!said){said=1;snprintf(t,sizeof(t),"pick alloc failed hdr=%d arena=%d gray=%d",!!pk_hdr,!!pk_arena,!!pk_gray);
            diag(t);diag_mem("pick");diag_flush();}
        return;}
    e=&frames[i];jpg=(e->flags&F_JPG)!=0;
    frame_path(e,!jpg,path,sizeof(path));
#define PK_FAIL(step,v) do{if(pk_logged<4){char t_[128];pk_logged++;snprintf(t_,sizeof(t_),"pick %s: %s (%d)",path,step,(int)(v));diag(t_);diag_flush();}return;}while(0)
    if(FIO_GetFileSize(path,&size)!=0)PK_FAIL("getsize",-1);
    if(size<64)PK_FAIL("too small",size);
    n=jpg?(size<64*1024?size:64*1024):(size<HDR_CAP?size:HDR_CAP);
    f=FIO_OpenFile(path,O_RDONLY);if(!good_file(f))PK_FAIL("open",(int)f);
    if(read_exact(f,0,pk_hdr,n)!=0){FIO_CloseFile(f);PK_FAIL("read n",n);}
    FIO_CloseFile(f);
    if(jpg&&has_tag(pk_hdr,n))e->done=1;    /* developed in place */
    read_meta(pk_hdr,n,!jpg,&m);
    if(!m.thumb_len)PK_FAIL("no exif thumb, head",(pk_hdr[0]<<8)|pk_hdr[1]);
    if(decode_thumb_gray(pk_hdr+m.thumb_off,m.thumb_len,pk_arena,64*1024,pk_gray,&pk_w,&pk_h)!=0)PK_FAIL("thumb decode len",m.thumb_len);
#undef PK_FAIL
    pk_orient=m.orient;pk_ok=1;
}

static void draw_picker(se_ui_fb *fb)
{
    char a[40],b[48];frame_ent *e;int x;
    if(nframes<1||pick_i<0||pick_i>=nframes){
        se_ui_chrome(fb,"PICK A FRAME",0,"[MENU] BACK");
        se_ui_text_c(fb,SE_UI_W/2,100,"NO PHOTOS IN THIS FOLDER",SE_C_RED,3,2);
        return;
    }
    e=&frames[pick_i];
    snprintf(a,sizeof(a),"%d OF %d",pick_i+1,nframes);
    se_ui_chrome(fb,"PICK A FRAME",a,"[\x01\x02] FRAME   [TOP DIAL] 10   [SET] DEVELOP   [MENU] BACK");
    /* the thumbnail box; the picture itself goes straight to the overlay */
    se_ui_fill(fb,PK_BX-2,PK_BY/2-1,PK_BW+4,PK_BH/2+2,SE_C_INK);
    se_ui_fill(fb,PK_BX,PK_BY/2,PK_BW,PK_BH/2,SE_C_SHADOW);
    if(pk_loaded!=pick_i)se_ui_text_c(fb,SE_UI_W/2,PK_BY/2+PK_BH/4-4,"LOADING",SE_C_EDGE,2,1);
    else if(!pk_ok)se_ui_text_c(fb,SE_UI_W/2,PK_BY/2+PK_BH/4-4,"NO PREVIEW",SE_C_EDGE,2,1);
    if(pick_i>0)se_ui_text(fb,150,86,SE_SYM_LEFT,SE_C_MID,3,2);
    if(pick_i<nframes-1)se_ui_text(fb,SE_UI_W-170,86,SE_SYM_RIGHT,SE_C_MID,3,2);
    snprintf(a,sizeof(a),"#%04d",e->num);
    se_ui_text_c(fb,SE_UI_W/2,160,a,SE_C_INK,3,2);
    snprintf(b,sizeof(b),"%s%s%s",(e->flags&F_CR2)?"CR2":"",(e->flags&F_CR2)&&(e->flags&F_JPG)?" + ":"",(e->flags&F_JPG)?"JPG":"");
    if(e->flags&F_DUP)snprintf(b,sizeof(b),"SEVERAL FILES - SKIPPED");
    x=se_ui_text_w(b,2);
    if(e->done){
        int w2=se_ui_text_w("   DEVELOPED",2);
        se_ui_text(fb,(SE_UI_W-x-w2)/2,184,b,SE_C_SLATE,2,1);
        se_ui_text(fb,(SE_UI_W-x-w2)/2+x,184,"   DEVELOPED",SE_C_OLIVE,2,1);
    }else se_ui_text_c(fb,SE_UI_W/2,184,b,SE_C_SLATE,2,1);
}

/* Settings (INFO) and the develop menu (SET) share the engine's settings
 * screen with port-supplied rows. */
static void gui_prepare(void)
{
    gui.stock=stock_count?stock_pos(selected_stock):0;
    snprintf(gui_info,sizeof(gui_info),"%s / %s%s",set_res==0?"FULL":"HALF",mode_names[set_mode],set_auto?" / AUTO":"");
    gui.info_label="OUTPUT: ";gui.info_value=gui_info;
    gui.info_colour=0;
    gui.value[SE_SET_SOURCE]=SE_SOURCE_PREVIEW;gui.value[SE_SET_RES]=set_res;
    if(gmode==G_SETTINGS){
        gui.screen=SE_SCREEN_SETTINGS;gui.set_n=3;gui.setting=gsetting;
        gui.settings_title=0;gui.settings_right=0;
        gui.set_label[0]="SIZE";      gui.set_value[0]=set_res==0?"FULL  5184 PX":"HALF  2592 PX";
        gui.set_help[0]="FULL TAKES ABOUT FOUR TIMES AS LONG";
        {static const char *const mode_help[M_COUNT]={
            "THE DEVELOPED JPEG REPLACES THE CAMERA JPEG AND RAW",
            "KEEPS THE RAW BUT PLAYBACK SHOWS THE RAW PREVIEW",
            "SAVED NEXT TO THE ORIGINALS UNDER A NEW NUMBER",
            "DELETES THE RAW ONLY IF A FULL-SIZE JPEG IS KEPT",
            "NEW JPEG IN /ML/SOUP FOLDER"};
        gui.set_label[1]="OUTPUT";gui.set_value[1]=mode_names[set_mode];gui.set_help[1]=mode_help[set_mode];}
        gui.set_label[2]="AUTO DEVELOP";gui.set_value[2]=set_auto?"ON":"OFF";
        gui.set_help[2]=set_auto?"DEVELOPS AUTOMATICALLY WHEN YOU STOP SHOOTING":"DEVELOP BY HAND WITH SET ON THE FILM SCREEN";
        gui.settings_hint="[DIAL] ROW   [\x01\x02] CHANGE   [SET] BACK";
    }else if(gmode==G_DEVMENU){
        gui.screen=SE_SCREEN_SETTINGS;gui.set_n=4;gui.setting=gdev;
        snprintf(gui_title,sizeof(gui_title),"DEVELOP");gui.settings_title=gui_title;
        snprintf(gui_right,sizeof(gui_right),"%s",stock_count?stock_names[selected_stock]:"");gui.settings_right=gui_right;
        snprintf(gui_v[0],24,"%d FRAMES",nframes);
        if(dev_latest>=0)snprintf(gui_v[1],24,"#%04d",frames[dev_latest].num);else snprintf(gui_v[1],24,"NONE");
        if(dev_new)snprintf(gui_v[2],24,"%d FRAME%s",dev_new,dev_new==1?"":"S");else snprintf(gui_v[2],24,"NONE");
        snprintf(gui_v[3],24,"\x01 %d \x02",lastn_choices[set_lastn]);
        gui.set_label[0]="PICK";    gui.set_value[0]=gui_v[0];gui.set_help[0]="CHOOSE PICTURE TO DEVELOP";
        gui.set_label[1]="LATEST";  gui.set_value[1]=gui_v[1];gui.set_help[1]="THE LAST PHOTO TAKEN";
        gui.set_label[2]="ALL NEW"; gui.set_value[2]=gui_v[2];gui.set_help[2]="EVERYTHING SINCE THE LAST DEVELOP";
        snprintf(gui_lasthelp,sizeof(gui_lasthelp),"THE LAST " SE_ITALIC "%d" SE_ITALIC " PHOTOS",lastn_choices[set_lastn]);
        gui.set_label[3]="LAST";    gui.set_value[3]=gui_v[3];gui.set_help[3]=gui_lasthelp;
        gui.settings_hint=gdev==3?"[DIAL] ROW   [\x01\x02] HOW MANY   [SET] GO   [MENU] BACK":"[DIAL] ROW   [SET] GO   [MENU] BACK";
    }else if(gmode==G_WORK){
        gui.screen=SE_SCREEN_WORKING;
        gui.percent=progress_pct;gui.stage=cancel_job?"CANCELLING":"DEVELOPING";
        gui.done=job_pos;gui.total=job_total;
        snprintf(gui.file,sizeof(gui.file),"#%04d",job_nums[job_pos<job_total?job_pos:0]);
    }else if(gmode==G_DONE){
        gui.screen=SE_SCREEN_DONE;
    }else gui.screen=SE_SCREEN_STOCK;
}

static void gui_finish_job(void)
{
    gui.done=job_ok;gui.last_seconds=(job_ms+500)/1000;
    gui.done_colour=0;gui.done_title="DEVELOPED";
    /* Canon's playback list only gains files Canon writes itself, or finds
     * when it re-catalogues at power-on: ours appear after a restart. So
     * does a JPEG ONLY frame (Canon still has it as a RAW+JPEG pair).
     * REPLACE JPEG frames never look developed in playback: the CR2 is shown. */
    if(job_ok&&job_total==1)
        snprintf(gui_note,sizeof(gui_note),"%s %s%s%s",job_mode<=M_INPLACE?"REPLACED":"SAVED",
            strrchr(last_out,'/')?strrchr(last_out,'/')+1:last_out,
            job_deleted&&job_mode!=M_ONE?", RAW DELETED":"",job_mode==M_INPLACE||job_out?"":" - RESTART CAMERA TO VIEW");
    else if(job_ok&&!job_failed)
        snprintf(gui_note,sizeof(gui_note),"%s%s",job_mode==M_ONE?"JPEGS REPLACED - RESTART CAMERA TO VIEW":
            job_mode==M_INPLACE?"JPEGS REPLACED":
            job_out?"SAVED TO ML/SOUP":"RESTART CAMERA TO VIEW IN PLAYBACK",job_deleted&&job_mode!=M_ONE?", RAWS DELETED":"");
    else if(job_ok){gui.done_title="PARTLY DONE";gui.done_colour=SE_C_AMBER;
        snprintf(gui_note,sizeof(gui_note),"%d FAILED, %d SKIPPED - SEE ML/SOUP/FIND.TXT",job_failed,job_skipped);}
    else if(cancel_job){gui.done_title="CANCELLED";gui.done_colour=SE_C_MID;snprintf(gui_note,sizeof(gui_note),"ORIGINALS UNCHANGED");}
    else if(job_result==-2){gui.done_title="NOTHING DONE";gui.done_colour=SE_C_RED;snprintf(gui_note,sizeof(gui_note),"FRAMES SKIPPED - SEE ML/SOUP/FIND.TXT");}
    else{gui.done_title="FAILED";gui.done_colour=SE_C_RED;snprintf(gui_note,sizeof(gui_note),"ORIGINALS UNCHANGED - SEE ML/SOUP/FIND.TXT");}
    gui.done_note=gui_note;
    gmode=G_DONE;
}

static void gui_run(int count)
{
    const char *why=begin_job(sel_buf,count);
    if(why){gui.done=0;gui.last_seconds=0;gui.done_title="NOT STARTED";gui.done_colour=SE_C_RED;
        snprintf(gui_note,sizeof(gui_note),"%s",why);gui.done_note=gui_note;gmode=G_DONE;}
    else gmode=G_WORK;
}

/* Everything that touches the card happens here, never in the key handler. */
static void gui_handle_request(void)
{
    int r=gui_req;gui_req=REQ_NONE;
    switch(r){
    case REQ_DEVMENU:
        if(index_build()!=0||nframes<1){gui.done=0;gui.last_seconds=0;gui.done_title="NO PHOTOS";gui.done_colour=SE_C_RED;
            snprintf(gui_note,sizeof(gui_note),"NOTHING IN %s",frames_dir);gui.done_note=gui_note;gmode=G_DONE;break;}
        dev_latest=latest_index();dev_new=select_new(sel_buf,MAX_JOB);
        gdev=0;gmode=G_DEVMENU;break;
    case REQ_LATEST: if(dev_latest>=0){sel_buf[0]=frames[dev_latest].num;gui_run(1);}break;
    case REQ_NEW:    gui_run(select_new(sel_buf,MAX_JOB));break;
    case REQ_LASTN:  gui_run(select_last(sel_buf,MAX_JOB,lastn_choices[set_lastn]));break;
    case REQ_PICKONE:if(pick_i>=0&&pick_i<nframes){sel_buf[0]=frames[pick_i].num;pick_free();gui_run(1);}break;
    }
    gui_dirty=1;
}

/* The engine's list, in dial order: favourites first. */
static void gui_fill_stocks(void)
{
    int i;
    for(i=0;i<stock_count;i++){
        int k=stock_ord[i];
        gui_stocks[i].name=stock_names[k];gui_stocks[i].desc=stock_descs[k];gui_stocks[i].fav=stock_fav[k];
    }
    se_ui_set_stocks(&gui,gui_stocks,stock_count);
    gui.stock=stock_count?stock_pos(selected_stock):0;
}

static void gui_task(void *unused)
{
    uint8_t *fb=0;se_ui_fb f;int waited,last=0,done_at=0;
    (void)unused;
    fb=tmp_malloc(SE_UI_FB_SIZE);   /* off Canon heap: see Err70 notes */
    if(!fb)goto out;
    f.px=fb;
    for(waited=0;!gui_menu_shown()&&waited<50;waited++)msleep(20);
    if(!loaded){
        gui.ready=0;gui_prepare();se_ui_draw(&gui,&f);gui_blit(fb);   /* LOADING */
        ensure_loaded();
    }
    gui_fill_stocks();gui.ready=1;
    gui_dirty=1;
    while(!gui_close&&gui_menu_shown()&&!ml_shutdown_requested&&!stop_worker){
        if(gui_restock){gui_restock=0;gui_fill_stocks();}
        /* opened by auto develop: the shutter takes priority (the job stops on
         * the same half-press), and the result shows for a moment, then goes */
        if(gui_auto){
            if(get_halfshutter_pressed()||lv)break;   /* Live View came back */
            if(gmode==G_DONE){if(!done_at)done_at=get_ms_clock();else if(get_ms_clock()-done_at>2500)break;}
        }
        if(gui_save){gui_save=0;save_cfg();}
        if(gui_req)gui_handle_request();
        if(busy&&gmode!=G_WORK){gmode=G_WORK;gui_dirty=1;}
        if(!busy&&gmode==G_WORK){gui_finish_job();gui_dirty=1;}
        if(gmode==G_PICK&&pk_loaded!=pick_i){
            draw_picker(&f);gui_blit(fb);           /* shows LOADING meanwhile */
            pick_load(pick_i);gui_dirty=1;          /* keys may move on; reloads next pass */
        }
        if(gmode!=G_PICK&&pk_loaded!=-2)pick_free();
        if(gui_dirty||(busy&&get_ms_clock()-last>300)){
            gui_dirty=0;
            if(gmode==G_PICK){
                draw_picker(&f);gui_blit(fb);
                if(pk_loaded==pick_i&&pk_ok)pick_blit_thumb(pk_gray,pk_w,pk_h,pk_orient);
            }else{gui_prepare();se_ui_draw(&gui,&f);gui_blit(fb);}
            last=get_ms_clock();
        }
        msleep(40);
    }
out:
    if(gui_save){gui_save=0;save_cfg();}
    pick_free();
    gui_running=0;gui_auto=0;
    if(gui_menu_shown()){
        clrscr();gui_stop_menu();
        for(waited=0;gui_menu_shown()&&waited<50;waited++)msleep(20);
    }
    menu_redraw_blocked=0;
    if(fb)free(fb);
    if(!busy)frames_release();      /* a running job still uses it; it frees it */
    gui_task_alive=0;
}

static int gui_open(void)
{
    if(gui_task_alive)return -1;
    se_ui_init(&gui);
    se_ui_set_stocks(&gui,gui_stocks,0);   /* filled by the GUI task */
    gui.ready=loaded;gui.stock=0;
    gui.stock_hint="[\x01\x02] FILM  [SET] DEVELOP  [Q] FAV  [INFO] SETUP  [MENU] CLOSE";
    gmode=busy?G_WORK:G_STOCK;gsetting=0;gdev=0;pick_i=0;
    gui_close=0;gui_req=REQ_NONE;gui_save=0;gui_dirty=1;pk_loaded=-2;
    gui_task_alive=1;gui_running=1;
    menu_redraw_blocked=1;           /* ML's menu must never paint over us */
    if(IS_ERROR(task_create("soup7d_gui",0x1c,0x4000,gui_task,0))){
        gui_running=0;gui_task_alive=0;menu_redraw_blocked=0;return -1;
    }
    /* ML paints audio meters over any open menu whose selected tab is
     * "Audio"; park its selection on our own Film tab first. */
    select_menu("Film",0);
    gui_open_menu();
    return 0;
}

/* dir: -1/+1 for joystick or rear dial, -10/+10 for the top dial */
static void gui_turn(int dir)
{
    int step=dir>0?1:-1;
    switch(gmode){
    case G_STOCK:
        if(stock_count){stock_step(step);gui_save=1;}
        break;
    case G_SETTINGS:
        if(gsetting==0)set_res=!set_res;
        else if(gsetting==1)set_mode=(set_mode+step+M_COUNT)%M_COUNT;
        else set_auto=!set_auto;
        break;
    case G_DEVMENU:
        if(gdev==3){int n=set_lastn+step;if(n>=0&&n<5)set_lastn=n;gui_save=1;}
        break;
    case G_PICK:
        {int n=pick_i+dir;if(n<0)n=0;if(n>=nframes)n=nframes-1;pick_i=n;}
        break;
    case G_DONE:
        gmode=G_STOCK;break;
    }
}
static void gui_row(int step)
{
    if(gmode==G_SETTINGS)gsetting=(gsetting+step+3)%3;
    else if(gmode==G_DEVMENU)gdev=(gdev+step+4)%4;
    else if(gmode==G_PICK)gui_turn(step);
}
static void gui_set(void)
{
    switch(gmode){
    case G_STOCK:   if(!busy&&!gui_req)gui_req=REQ_DEVMENU;break;
    case G_SETTINGS:gmode=G_STOCK;gui_save=1;break;
    case G_DEVMENU:
        if(gui_req)break;
        if(gdev==0){gmode=G_PICK;pick_i=dev_latest>=0?dev_latest:nframes-1;}
        else if(gdev==1)gui_req=REQ_LATEST;else if(gdev==2)gui_req=REQ_NEW;else gui_req=REQ_LASTN;
        break;
    case G_PICK:    if(!gui_req)gui_req=REQ_PICKONE;break;
    case G_DONE:    gmode=G_STOCK;break;
    }
}
static void gui_back(void)
{
    switch(gmode){
    case G_WORK:    cancel_job=1;break;           /* MENU cancels a running job */
    case G_SETTINGS:gmode=G_STOCK;gui_save=1;break;
    case G_DEVMENU: gmode=G_STOCK;break;
    case G_PICK:    gmode=G_DEVMENU;break;
    case G_DONE:    gmode=G_STOCK;break;
    default:        gui_close=1;break;
    }
}

static unsigned int soup_keypress(unsigned int key)
{
    if(!gui_running){
        if(key!=MODULE_KEY_PICSTYLE)return 1;
        /* leave Live View's arrow shortcut and playback zebras to ML */
        if(gui_menu_shown()||lv||is_pure_play_photo_or_movie_mode()||ml_shutdown_requested)return 1;
        gui_auto=0;
        return gui_open()==0?0:1;
    }
    switch(key){
    /* 7D: rear dial = WHEEL_UP/DOWN, top dial = WHEEL_LEFT/RIGHT */
    case MODULE_KEY_PRESS_LEFT: gui_turn(-1);break;
    case MODULE_KEY_PRESS_RIGHT:gui_turn(1);break;
    case MODULE_KEY_WHEEL_UP:   if(gmode==G_DEVMENU||gmode==G_SETTINGS)gui_row(-1);else gui_turn(-1);break;
    case MODULE_KEY_WHEEL_DOWN: if(gmode==G_DEVMENU||gmode==G_SETTINGS)gui_row(1);else gui_turn(1);break;
    case MODULE_KEY_WHEEL_LEFT: gui_turn(gmode==G_PICK?-10:-1);break;
    case MODULE_KEY_WHEEL_RIGHT:gui_turn(gmode==G_PICK?10:1);break;
    case MODULE_KEY_PRESS_UP:   gui_row(-1);break;
    case MODULE_KEY_PRESS_DOWN: gui_row(1);break;
    case MODULE_KEY_PRESS_SET:case MODULE_KEY_JOY_CENTER: gui_set();break;
    case MODULE_KEY_INFO:
        if(gmode==G_STOCK&&!busy){gmode=G_SETTINGS;gsetting=0;}
        else if(gmode==G_SETTINGS){gmode=G_STOCK;gui_save=1;}
        break;
    case MODULE_KEY_MENU: gui_back();break;
    case MODULE_KEY_Q:
        /* favourite on/off (the 7D has no RATE button); the stock stays
         * selected and moves with its group */
        if(gmode==G_STOCK&&stock_count&&!gui_restock){
            stock_fav[selected_stock]=!stock_fav[selected_stock];
            order_build();gui_restock=1;gui_save=1;
        }
        break;
    case MODULE_KEY_PICSTYLE:case MODULE_KEY_TRASH:
        if(gmode==G_SETTINGS)gui_save=1;
        gui_close=1;break;                        /* a running job carries on */
    /* swallow the rest of the buttons so the hidden ML menu never acts */
    case MODULE_KEY_PRESS_UP_RIGHT:case MODULE_KEY_PRESS_UP_LEFT:
    case MODULE_KEY_PRESS_DOWN_RIGHT:case MODULE_KEY_PRESS_DOWN_LEFT:
    case MODULE_KEY_UNPRESS_UDLR:case MODULE_KEY_UNPRESS_SET:
    case MODULE_KEY_PLAY:case MODULE_KEY_PRESS_ZOOMIN:
    case MODULE_KEY_RATE:case MODULE_KEY_LV:
        break;
    default:
        return 1;   /* shutter, internal events: pass through untouched */
    }
    gui_auto=0;     /* a button was used: the screen stays, like any other time */
    gui_dirty=1;
    return 0;
}

/* ======================================================== ML Film menu ===
 * Fallback for everything the Soup screen does that matters most. */
static MENU_SELECT_FUNC(menu_develop_latest)
{
    const char *why;int i;
    (void)priv;(void)delta;
    ensure_loaded();
    if(busy){NotifyBox(3000,"Soup: busy");return;}
    if(index_build()!=0||(i=latest_index())<0){NotifyBox(3000,"Soup: no photos in this folder");return;}
    sel_buf[0]=frames[i].num;
    why=begin_job(sel_buf,1);
    if(why)NotifyBox(3000,"Soup: %s",why);
}
static void ensure_loaded(void)
{
    if(loaded)return;
    loaded=1;
    diag_len=0;diag_buf[0]=0;
    diag_mem("ensure_loaded begin");
    load_stocks();load_cfg();
    {char t[48];snprintf(t,sizeof(t),"stocks loaded=%d",stock_count);diag(t);}
    diag_mem("ensure_loaded end");
    diag_flush();
    if(!stock_count){stock_choices[0]="No ML/FILM stocks";snprintf(status_text,sizeof(status_text),"Film stocks missing");}
    if(selected_stock>=stock_count)selected_stock=0;
}
static MENU_SELECT_FUNC(stock_select)
{
    (void)priv;ensure_loaded();
    if(stock_count){stock_step(delta<0?-1:1);save_cfg();}
}
static MENU_UPDATE_FUNC(show_stock)
{
    (void)entry;
    if(!loaded)MENU_SET_VALUE("press to load");
    else MENU_SET_VALUE("%s%s",stock_count&&stock_fav[selected_stock]?"* ":"",stock_count?stock_names[selected_stock]:"none in ML/FILM");
}
static MENU_SELECT_FUNC(toggle_res){(void)priv;(void)delta;ensure_loaded();set_res=!set_res;save_cfg();}
static MENU_UPDATE_FUNC(show_res){(void)entry;MENU_SET_VALUE("%s",set_res==0?"Full":"Half");}
static MENU_SELECT_FUNC(cycle_mode){(void)priv;ensure_loaded();set_mode=(set_mode+(delta<0?-1:1)+M_COUNT)%M_COUNT;save_cfg();}
static MENU_UPDATE_FUNC(show_mode){(void)entry;MENU_SET_VALUE("%s",mode_names[set_mode]);}
static MENU_SELECT_FUNC(toggle_auto){(void)priv;(void)delta;ensure_loaded();set_auto=!set_auto;save_cfg();}
static MENU_UPDATE_FUNC(show_auto){(void)entry;MENU_SET_VALUE("%s",set_auto?"ON":"OFF");}
static MENU_UPDATE_FUNC(show_status){(void)entry;MENU_SET_VALUE("%s",status_text);}
static struct menu_entry soup_menu[]={
    {.name="Film stock",.select=stock_select,.update=show_stock,
     .help="Soup Engine look for the next development."},
    {.name="Develop latest",.select=menu_develop_latest,
     .help="Or press Picture Style (outside LiveView) for the Soup screen."},
    {.name="Output size",.select=toggle_res,.update=show_res,.help="Full takes about four times as long as half."},
    {.name="Output",.select=cycle_mode,.update=show_mode,
     .help="JPEG ONLY: the developed JPEG replaces Canon's JPEG and the RAW.",
     .help2="REPLACE JPEG keeps the RAW. NEW JPEG modes keep the originals."},
    {.name="Auto develop",.select=toggle_auto,.update=show_auto,
     .help="Develops automatically when you stop shooting."},
    {.name="Soup status",.update=show_status,.help="Experimental 7D 2.0.3 module."},
};
static unsigned int soup_init(void)
{
    if(!is_camera("7D","2.0.3"))return 1;
    /* no card I/O here -- see ensure_loaded() */
    menu_add("Film",soup_menu,COUNT(soup_menu));return 0;
}
static unsigned int soup_deinit(void)
{
    stop_worker=1;
    /* Shutdown must not return while our tasks are using the card/module. */
    menu_remove("Film",soup_menu,COUNT(soup_menu));
    while(busy||gui_task_alive)msleep(20);
    menu_redraw_blocked=0;
    if(frames){free(frames);frames=0;}
    return 0;
}
MODULE_INFO_START()
    MODULE_INIT(soup_init)
    MODULE_DEINIT(soup_deinit)
    MODULE_LONGNAME("Soup Engine 7D prototype")
MODULE_INFO_END()
MODULE_CBRS_START()
    MODULE_CBR(CBR_KEYPRESS, soup_keypress, 0)
    MODULE_CBR(CBR_SECONDS_CLOCK, soup_clock, 0)
MODULE_CBRS_END()
