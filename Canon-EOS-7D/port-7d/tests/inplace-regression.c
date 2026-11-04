/* Host test of the real develop path (head.c, part_frames.c, part_meta.c,
 * part_job.c) on real 7D files, against tests/mlstub. Covers the output
 * modes and auto develop:
 *   ONE JPEG   frame's JPG replaced by a tagged develop, CR2 gone, no TMP;
 *              developing it again is refused (no CR2 left to start from)
 *   IN PLACE   JPG replaced, CR2 kept; developing again starts from the CR2
 *   NEW FILE   SOUP####.JPG beside untouched originals
 *   AUTO       a new shot is developed once the counter has been quiet; a
 *              half-press stops it with the frame untouched, then it resumes
 * usage: inplace-regression <folder of 7D CR2/JPG> <card/ML/FILM> */
#include "mlstub/module.h"
char STUB_ROOT[256];
struct lens_info_stub lens_info;
struct card_info stub_card;
int ml_shutdown_requested,lv,menu_redraw_blocked,stub_half,stub_half_after;
#include "../parts/head.c"
#include "../parts/part_frames.c"
#include "../parts/part_meta.c"
#include "../parts/part_job.c"
#include <assert.h>
static void ensure_loaded(void){ if(loaded)return; loaded=1; load_stocks(); load_cfg(); }
static int gui_open_calls;
static int gui_open(void){ gui_open_calls++; return -1; }   /* the GUI is not part of this test */
static const char *SM,*FM;
static void copy(const char *src,const char *dst_rel){
    char d[512],cmd[1400];snprintf(d,sizeof d,"%s/%s",STUB_ROOT,dst_rel);
    snprintf(cmd,sizeof cmd,"cp '%s' '%s'",src,d);assert(system(cmd)==0);
}
static void frame(int n,int cr2,int jpg){
    char s[512],r[64];
    if(cr2){snprintf(s,sizeof s,"%s/_B7D0526.CR2",SM);snprintf(r,sizeof r,"DCIM/100EOS7D/_B7D%04d.CR2",n);copy(s,r);}
    if(jpg){snprintf(s,sizeof s,"%s/_B7D0526.JPG",SM);snprintf(r,sizeof r,"DCIM/100EOS7D/_B7D%04d.JPG",n);copy(s,r);}
}
static int exists(const char *rel){char p[300];snprintf(p,sizeof p,"A:/DCIM/100EOS7D/%s",rel);return is_file(p);}
static uint32_t size_of(const char *rel){char p[300];uint32_t s=0;snprintf(p,sizeof p,"A:/DCIM/100EOS7D/%s",rel);FIO_GetFileSize(p,&s);return s;}
static int tagged(const char *rel){char p[300];uint8_t b[1024];snprintf(p,sizeof p,"A:/DCIM/100EOS7D/%s",rel);return soup_marked(p,b,1024);}
static void dims(const char *rel,int *w,int *h){
    char p[300];uint32_t s=0;FILE *f;uint8_t *b;se_jdec_info ji;
    snprintf(p,sizeof p,"A:/DCIM/100EOS7D/%s",rel);FIO_GetFileSize(p,&s);
    b=malloc(s);f=FIO_OpenFile(p,0);assert(f&&read_exact(f,0,b,s)==0);FIO_CloseFile(f);
    assert(b[0]==0xFF&&b[1]==0xD8&&b[s-2]==0xFF&&b[s-1]==0xD9);
    assert(se_jpeg_dec_info(b,s,&ji)==SE_OK);*w=ji.width;*h=ji.height;free(b);
}
static void run(int num){
    uint16_t n[1];n[0]=(uint16_t)num;
    assert(index_build()==0);assert(begin_job(n,1)==0);   /* job runs synchronously */
}
int main(int argc,char **argv){
    char cmd[600];int w,h;uint32_t s1;
    assert(argc==3);SM=argv[1];FM=argv[2];
    snprintf(STUB_ROOT,sizeof STUB_ROOT,"/tmp/soup-inplace-%d",(int)getpid());
    snprintf(cmd,sizeof cmd,"rm -rf %s && mkdir -p %s/DCIM/100EOS7D %s/ML/FILM %s/ML/SOUP %s/ML/SETTINGS",
        STUB_ROOT,STUB_ROOT,STUB_ROOT,STUB_ROOT,STUB_ROOT);assert(system(cmd)==0);
    {char s[512];snprintf(s,sizeof s,"%s/tmax.fs",FM);copy(s,"ML/FILM/tmax.fs");}
    {FILE *f;char p[512];snprintf(p,sizeof p,"%s/ML/FILM/INDEX.TXT",STUB_ROOT);f=fopen(p,"w");fputs("tmax.fs\n",f);fclose(f);}
    ensure_loaded();assert(stock_count==1);
    assert(set_mode==M_ONE&&set_auto==0);                 /* the defaults */
    set_res=1;

    /* ONE JPEG on RAW+JPEG L */
    frame(10,1,1);stub_card.file_number=10;
    run(10);
    assert(job_ok==1&&job_deleted==1);
    assert(exists("_B7D0010.JPG")&&!exists("_B7D0010.CR2")&&!exists("SOUI0010.TMP"));
    assert(tagged("_B7D0010.JPG"));dims("_B7D0010.JPG",&w,&h);assert(w==2592&&h==1728);
    s1=size_of("_B7D0010.JPG");
    /* again: our own JPG is never a source, and there is no CR2 left */
    run(10);assert(job_ok==0&&job_skipped==1&&size_of("_B7D0010.JPG")==s1);
    puts("ONE JPEG: replaced, RAW deleted, second develop refused");

    /* IN PLACE keeps the RAW; a second develop starts again from the CR2 */
    set_mode=M_INPLACE;frame(11,1,1);run(11);
    assert(job_ok==1&&job_deleted==0&&exists("_B7D0011.CR2")&&tagged("_B7D0011.JPG"));
    run(11);assert(job_ok==1&&tagged("_B7D0011.JPG")&&exists("_B7D0011.CR2"));
    {char *fl=strstr(job_log,"from=");assert(fl&&strstr(fl,".CR2"));}
    puts("IN PLACE: replaced, RAW kept, re-develop from CR2");

    /* RAW only, ONE JPEG: a JPG appears under the CR2's prefix, CR2 gone */
    set_mode=M_ONE;frame(12,1,0);run(12);
    assert(job_ok==1&&exists("_B7D0012.JPG")&&!exists("_B7D0012.CR2")&&tagged("_B7D0012.JPG"));
    puts("ONE JPEG, RAW only: JPG under the CR2 name, CR2 deleted");

    /* NEW FILE leaves both originals */
    set_mode=M_NEW;frame(13,1,1);s1=size_of("_B7D0013.JPG");run(13);
    assert(job_ok==1&&exists("_B7D0013.CR2")&&size_of("_B7D0013.JPG")==s1&&!tagged("_B7D0013.JPG"));
    assert(strstr(last_out,"/SOUP")!=0);
    puts("NEW FILE: SOUP file, originals untouched");

    /* AUTO, ONE JPEG */
    set_mode=M_ONE;set_auto=1;
    stub_card.file_number=13;auto_base=-1;soup_clock(0);   /* boot: baseline */
    frame(14,1,1);stub_card.file_number=14;
    soup_clock(0);                         /* counter moved: wait */
    assert(!tagged("_B7D0014.JPG"));
    soup_clock(0);                         /* quiet 1 */
    assert(!tagged("_B7D0014.JPG"));
    stub_half=1;soup_clock(0);             /* half-pressed: nothing starts */
    assert(!tagged("_B7D0014.JPG")&&exists("_B7D0014.CR2"));
    gui_open_calls=0;
    stub_half=0;soup_clock(0);             /* quiet and idle: develops */
    assert(tagged("_B7D0014.JPG")&&!exists("_B7D0014.CR2")&&auto_base==14);
    assert(gui_open_calls==1&&gui_auto==0);   /* asked for the developing screen once */
    soup_clock(0);soup_clock(0);soup_clock(0);
    assert(auto_base==14&&gui_open_calls==1); /* nothing new: no screen */
    puts("AUTO: waits for quiet + no half-press, then develops in place");

    /* AUTO, half-press arriving mid-develop: stopped, frame untouched, tried again */
    frame(16,1,1);s1=size_of("_B7D0016.JPG");stub_card.file_number=16;
    soup_clock(0);soup_clock(0);
    stub_half_after=60;soup_clock(0);      /* starts, then the shutter is half-pressed */
    assert(!tagged("_B7D0016.JPG")&&size_of("_B7D0016.JPG")==s1&&exists("_B7D0016.CR2"));
    assert(!exists("SOUI0016.TMP")&&auto_base==14);
    stub_half=0;stub_half_after=0;soup_clock(0);
    assert(tagged("_B7D0016.JPG")&&!exists("_B7D0016.CR2")&&auto_base==16);
    puts("AUTO: half-press mid-develop stops it cleanly, then it resumes");
    gui_open_calls=0;
    lv=1;frame(17,1,1);stub_card.file_number=17;soup_clock(0);soup_clock(0);soup_clock(0);
    assert(tagged("_B7D0017.JPG")&&gui_open_calls==0);   /* Live View: develops, no screen */
    /* the camera case: lv reads 0 while a Live View shot is captured and reviewed */
    soup_clock(0);lv=0;frame(18,1,1);stub_card.file_number=18;soup_clock(0);soup_clock(0);soup_clock(0);
    assert(tagged("_B7D0018.JPG")&&gui_open_calls==0);
    puts("AUTO in Live View: develops, no screen (also while lv reads 0 after the shot)");

    /* AUTO off: the counter is followed, nothing developed */
    set_auto=0;frame(15,1,1);stub_card.file_number=15;soup_clock(0);soup_clock(0);soup_clock(0);
    assert(!tagged("_B7D0015.JPG")&&exists("_B7D0015.CR2")&&auto_base==15);
    puts("AUTO off: nothing touched");

    /* settings round trip */
    set_mode=M_INPLACE;set_auto=1;set_res=0;save_cfg();
    set_mode=M_ONE;set_auto=0;set_res=1;load_cfg();
    assert(set_mode==M_INPLACE&&set_auto==1&&set_res==0&&selected_stock==0);

    /* FAVOURITES: first on the dial in INDEX order, kept by file name */
    {char src[512];FILE *f;char p[512];
     snprintf(src,sizeof src,"%s/gold.fs",FM);copy(src,"ML/FILM/gold.fs");
     snprintf(src,sizeof src,"%s/hp5.fs",FM);copy(src,"ML/FILM/hp5.fs");
     snprintf(p,sizeof p,"%s/ML/FILM/INDEX.TXT",STUB_ROOT);f=fopen(p,"w");fputs("gold.fs\nhp5.fs\ntmax.fs\n",f);fclose(f);}
    loaded=0;stock_count=0;selected_stock=0;memset(stock_fav,0,sizeof stock_fav);ensure_loaded();
    assert(stock_count==3);
    assert(!strcmp(stock_paths[selected_stock]+8,"tmax.fs"));       /* film= survived the new INDEX */
    assert(stock_ord[0]==0&&stock_ord[1]==1&&stock_ord[2]==2);       /* no favourites: INDEX order */
    stock_fav[2]=1;order_build();                                     /* tmax */
    assert(stock_ord[0]==2&&stock_ord[1]==0&&stock_ord[2]==1&&stock_pos(2)==0);
    stock_fav[1]=1;order_build();                                     /* + hp5 */
    assert(stock_ord[0]==1&&stock_ord[1]==2&&stock_ord[2]==0);
    selected_stock=1;stock_step(1);assert(selected_stock==2);         /* hp5 -> tmax */
    stock_step(1);assert(selected_stock==0);stock_step(1);assert(selected_stock==1);   /* wraps */
    stock_step(-1);assert(selected_stock==0);
    save_cfg();
    {char p[512],t[640];FILE *f;size_t n;snprintf(p,sizeof p,"%s/ML/SETTINGS/SOUP7D.CFG",STUB_ROOT);
     f=fopen(p,"rb");n=fread(t,1,sizeof t-1,f);fclose(f);t[n]=0;
     assert(strstr(t,"fav=hp5.fs,tmax.fs\r\n")&&strstr(t,"film=gold.fs\r\n"));}
    memset(stock_fav,0,sizeof stock_fav);selected_stock=2;load_cfg();
    assert(stock_fav[0]==0&&stock_fav[1]==1&&stock_fav[2]==1&&selected_stock==0);
    assert(stock_ord[0]==1&&stock_ord[1]==2&&stock_ord[2]==0);
    stock_fav[1]=stock_fav[2]=0;save_cfg();memset(stock_fav,1,sizeof stock_fav);load_cfg();
    assert(!stock_fav[0]&&!stock_fav[1]&&!stock_fav[2]);             /* empty fav= clears */
    puts("FAVOURITES: order, step, wrap, saved by file name");

    snprintf(cmd,sizeof cmd,"rm -rf %s",STUB_ROOT);system(cmd);
    puts("inplace-regression: all checks passed");return 0;
}
