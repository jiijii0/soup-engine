/* Host regression for the 7D batch/picker build. Includes the production
 * parts directly (frames, meta) and the generated graded-thumb slice. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include "se.h"
#include "stock.h"
#include "look.h"
#include "jpeg_dec.h"
#include "jpeg_enc.h"
/* --- ML stubs ---------------------------------------------------------- */
#define FIO_MAX_PATH_LENGTH 0x80
#define ATTR_DIRECTORY 0x10
#define IS_ERROR(p) ((uintptr_t)(p) & 1)
struct fio_dirent { int i; const char **l; };
struct fio_file { uint32_t mode,size,timestamp,off_0x0c; char name[FIO_MAX_PATH_LENGTH]; uint32_t a,b,c,d; };
struct card_info { int file_number; };
static volatile int stop_worker,cancel_job; static int ml_shutdown_requested;
#define JOB_STOP() (stop_worker||ml_shutdown_requested||cancel_job)
static const char *dcim_list[64], *soup_list[16]; static int open_dirs; static struct card_info card;
static struct fio_dirent dirs[4];
static const char *get_dcim_dir(void){ return "A:/DCIM/100EOS7D"; }
static struct card_info *get_shooting_card(void){ return &card; }
static int fill(struct fio_dirent *d,struct fio_file *f){ if(!d->l[d->i])return -1; memset(f,0,sizeof *f);
  const char *n=d->l[d->i++]; if(n[0]=='/'){f->mode=ATTR_DIRECTORY;n++;} snprintf(f->name,sizeof f->name,"%s",n); return 0; }
static struct fio_dirent *FIO_FindFirstEx(const char *dir,struct fio_file *f){
  const char **l=!strcmp(dir,"A:/DCIM/100EOS7D")?dcim_list:!strcmp(dir,"ML/SOUP")?soup_list:0;
  if(!l)return (struct fio_dirent*)1; struct fio_dirent *d=&dirs[open_dirs++]; d->i=0; d->l=l;
  if(fill(d,f))memset(f,0,sizeof *f); return d; }
static int FIO_FindNextEx(struct fio_dirent *d,struct fio_file *f){ return fill(d,f); }
static void FIO_FindClose(struct fio_dirent *d){ (void)d; open_dirs--; }
#include "ml-file-stubs.h"
#include "../parts/part_frames.c"
#include "../parts/part_meta.c"
#include "gen-graded-thumb.inc"

static uint8_t *slurp(const char *p,uint32_t *n){FILE*f=fopen(p,"rb");assert(f);fseek(f,0,SEEK_END);*n=ftell(f);fseek(f,0,SEEK_SET);
  uint8_t*b=malloc(*n);assert(fread(b,1,*n,f)==*n);fclose(f);return b;}
static void put_pgm(const char *p,const uint8_t *g,int w,int h){FILE*f=fopen(p,"wb");fprintf(f,"P5\n%d %d\n255\n",w,h);
  for(int y=0;y<h;y++)fwrite(g+y*THUMB_W,1,w,f);fclose(f);}
typedef struct {FILE *f;} fout; static int fsink(void*c,const uint8_t*d,size_t n){return fwrite(d,1,n,((fout*)c)->f)==n?0:-1;}
typedef struct {se_jpeg *enc;int width;} plain; static int prow(void*c,const uint8_t*rgb,int n,int w){plain*p=c;return se_jpeg_write_rows(p->enc,rgb,n)==SE_OK?0:-1;}

int main(int argc,char**argv){
  const char *S=argv[1],*stockf=argv[2];
  uint32_t n; char path[256];
  /* ---- EXIF/thumbnail on the real files ---- */
  meta_info m; uint8_t gray[THUMB_W*THUMB_H]; static uint8_t arena[64*1024]; int w,h;
  snprintf(path,sizeof path,"%s/_B7D0526.CR2",S); uint8_t *cr2=slurp(path,&n);
  read_meta(cr2,n<256*1024?n:256*1024,1,&m);
  printf("CR2: thumb@%u len=%u orient=%d make=%s model=%s date=%s exp=%.5f f=%.1f iso=%d\n",m.thumb_off,m.thumb_len,m.orient,m.make,m.model,m.date,m.exposure,m.fnumber,m.iso);
  assert(m.thumb_off==57256&&m.thumb_len==14727&&m.orient==1&&!strcmp(m.make,"Canon"));
  assert(decode_thumb_gray(cr2+m.thumb_off,m.thumb_len,arena,sizeof arena,gray,&w,&h)==0);
  printf("CR2 thumb decoded %dx%d\n",w,h); put_pgm("thumb-cr2.pgm",gray,w,h);
  snprintf(path,sizeof path,"%s/_B7D0526.JPG",S); uint32_t jn; uint8_t *jpg=slurp(path,&jn);
  meta_info mj; read_meta(jpg,64*1024,0,&mj);
  printf("JPG: thumb@%u len=%u orient=%d model=%s date=%s exp=%.5f f=%.1f iso=%d\n",mj.thumb_off,mj.thumb_len,mj.orient,mj.model,mj.date,mj.exposure,mj.fnumber,mj.iso);
  assert(mj.thumb_off==12+11432&&mj.thumb_len==14727&&!strcmp(mj.date,m.date));
  assert(decode_thumb_gray(jpg+mj.thumb_off,mj.thumb_len,arena,sizeof arena,gray,&w,&h)==0&&w==160&&h==120);
  /* truncated buffers must not read out of bounds or claim a thumbnail */
  for(uint32_t cut=0;cut<80*1024;cut+=997){ meta_info t; read_meta(jpg,cut,0,&t); read_meta(cr2,cut,1,&t);
    if(t.thumb_len) assert(t.thumb_off+t.thumb_len<=cut); }
  /* ---- graded thumbnail ---- */
  char text[16384]; FILE *sf=fopen(stockf,"rb"); size_t got=fread(text,1,sizeof text,sf); fclose(sf);
  se_stock st; se_stock_init(&st); assert(se_stock_parse(text,got,&st)==SE_OK);
  uint8_t th[TH_CAP]; int tl=graded_thumb(&st,jpg+mj.thumb_off,mj.thumb_len,th,5184);
  printf("graded thumb %d bytes (cap %d)\n",tl,TH_CAP); assert(tl>500&&tl<=TH_CAP);
  { se_jdec_info ji; assert(se_jpeg_dec_info(th,tl,&ji)==SE_OK); printf("graded thumb %dx%d\n",ji.width,ji.height); }
  /* ---- an output carrying EXIF: orientation, thumbnail, exposure ---- */
  { se_jpeg_exif ex; memset(&ex,0,sizeof ex); ex.make="Canon";ex.model=mj.model;ex.software="SOUP ENGINE - TEST";
    ex.datetime=mj.date;ex.exposure_time=mj.exposure;ex.aperture=mj.fnumber;ex.iso=mj.iso;ex.orientation=6;ex.thumb=th;ex.thumb_len=tl;
    static uint8_t mem[512*1024]; se_arena a; se_arena_init(&a,mem,sizeof mem);
    plain p; p.enc=SE_NEW(&a,se_jpeg,1); uint8_t *strip=SE_NEW(&a,uint8_t,8*160*3); fout fo; fo.f=fopen("exif-out.jpg","wb");
    se_jdec_info ji; assert(se_jpeg_begin(p.enc,160,120,88,strip,&ex,fsink,&fo)==SE_OK);
    assert(se_jpeg_decode_scaled(jpg+mj.thumb_off,mj.thumb_len,1,&a,prow,&p,&ji)==SE_OK&&se_jpeg_end(p.enc)==SE_OK); fclose(fo.f);
    uint32_t on; uint8_t *o=slurp("exif-out.jpg",&on); meta_info mo; read_meta(o,on,0,&mo);
    printf("output EXIF: orient=%d thumb=%u model=%s date=%s iso=%d\n",mo.orient,mo.thumb_len,mo.model,mo.date,mo.iso);
    assert(mo.orient==6&&mo.thumb_len==(uint32_t)tl&&!strcmp(mo.date,mj.date)&&mo.iso==mj.iso); free(o); }
  /* ---- frame index and selections ---- */
  const char *L1[]={"/.","SB7D0100.CR2","SB7D0100.JPG","_B7D0525.CR2","_B7D0526.JPG","_B7D0527.CR2","_B7D0527.JPG",
                    "_B7D0528.JPG","SOUP0526.JPG","SOU20526.JPG","_B7D0529.CR2","SB7D0529.CR2","_B7D0530.JPG","NOTES.TXT",0};
  memcpy(dcim_list,L1,sizeof L1); soup_list[0]=0; card.file_number=530;
  assert(index_build()==0&&open_dirs==0);
  assert(nframes==7);                          /* 100,525,526,527,528,529,530 ; SOUP files are not frames */
  int i26=frame_lookup(526); assert(i26>=0&&frames[i26].done==1&&frames[i26].flags==F_JPG);
  assert(frames[frame_lookup(529)].flags&F_DUP);
  assert(latest_index()==nframes-1&&frames[latest_index()].num==530);
  uint16_t sel[MAX_FRAMES]; int k=select_new(sel,200);
  assert(k==4&&sel[0]==527&&sel[3]==530);      /* after the newest developed (526) */
  k=select_last(sel,200,3); assert(k==3&&sel[0]==528&&sel[2]==530);
  char p[96]; frame_path(&frames[frame_lookup(527)],1,p,sizeof p); assert(!strcmp(p,"A:/DCIM/100EOS7D/_B7D0527.CR2"));
  /* nothing developed yet: ALL NEW is only the latest, never the whole card */
  const char *L2[]={"_B7D0001.JPG","_B7D0002.JPG","_B7D0003.JPG",0}; memcpy(dcim_list,L2,sizeof L2); card.file_number=2;
  assert(index_build()==0); k=select_new(sel,200); assert(k==1&&sel[0]==2);
  /* outputs kept in ML/SOUP count as developed too */
  soup_list[0]="SOUP0002.JPG"; soup_list[1]="SOUP0999.JPG"; soup_list[2]=0;
  assert(index_build()==0&&nframes==3); k=select_new(sel,200); assert(k==1&&sel[0]==3);
  /* unreadable folder */
  dcim_list[0]=0; assert(index_build()==0&&nframes==0&&select_new(sel,200)==0&&select_last(sel,200,5)==0);
  puts("batch-regression: all checks passed"); return 0;
}
