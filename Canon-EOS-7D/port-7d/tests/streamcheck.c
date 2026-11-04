/* Targeted host check of the module's streaming pipeline and memory budgets. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "se.h"
#include "cr2.h"
#include "stock.h"
#include "look.h"
#include "jpeg_dec.h"
#include "jpeg_enc.h"
#define HDR (256u*1024u)
#define SCRATCH (768u*1024u)
#define WINDOW 8192u
typedef struct {FILE *in,*out;uint32_t base,total,hash;unsigned reads;se_jpeg *enc;se_look_run *look;int width;} ctx;
static int rd(void *p,uint32_t off,void *dst,uint32_t n){ctx *c=p;if(n>WINDOW||off>c->total||n>c->total-off)return -1;c->reads++;return fseek(c->in,c->base+off,SEEK_SET)==0&&fread(dst,1,n,c->in)==n?0:-1;}
static int wr(void *p,const uint8_t *d,size_t n){return fwrite(d,1,n,((ctx*)p)->out)==n?0:-1;}
static int emit(void *p,const uint8_t *row,int width){ctx*c=p;if(width!=c->width)return -1;for(int i=0;i<width*3;i++){c->hash^=row[i];c->hash*=16777619u;}return se_jpeg_write_rows(c->enc,row,1)==SE_OK?0:-1;}
static int rows(void *p,const uint8_t *rgb,int n,int width){ctx*c=p;if(width!=c->width)return -1;return se_look_rows(c->look,rgb,n,emit,c)==SE_OK?0:-1;}
int main(int argc,char**argv){
 /* argv: input stock output [scale 1|2|4] [jpg]; defaults keep the original check */
 int scale=argc>4?atoi(argv[4]):2,isjpg=argc>5&&!strcmp(argv[5],"jpg");
 if(argc<4||argc>6||(scale!=1&&scale!=2&&scale!=4))return 2;
 ctx c={0};se_stock st;se_look look;se_arena arena,la;se_jdec_info ji;uint32_t off,len;int pw,ph,ow,oh;
 uint8_t *header=malloc(HDR),*memory,*strip;char text[16384];FILE*sf;size_t got,ls,hdrlen;int rc=2;
 c.in=fopen(argv[1],"rb");if(!c.in)return 2;
 got=fread(header,1,HDR,c.in);
 if(isjpg){fseek(c.in,0,SEEK_END);len=(uint32_t)ftell(c.in);off=0;pw=ph=0;if(!got)goto done;}
 else{if(got!=HDR)goto done;
 if(se_cr2_find_preview(header,got,&off,&len,&pw,&ph)!=SE_OK||off>=HDR)goto done;}
 hdrlen=got-off;if(hdrlen>len)hdrlen=len;
 if(se_jpeg_dec_info(header+off,hdrlen,&ji)!=SE_OK)goto done;
 sf=fopen(argv[2],"rb");if(!sf)goto done;got=fread(text,1,sizeof text,sf);fclose(sf);
 if(got==sizeof text)goto done;se_stock_init(&st);if(se_stock_parse(text,got,&st)!=SE_OK)goto done;
 se_jpeg_scaled_size(ji.width,ji.height,scale,&ow,&oh);se_look_from_stock(&st,scale,&look);ls=se_look_fit(&look,ow,SE_LOOK_BUDGET);
 memory=malloc(SCRATCH+ls+64);if(!memory)goto done;
 se_arena_init(&arena,memory,SCRATCH);se_arena_init(&la,memory+SCRATCH,ls);
 c.enc=SE_NEW(&arena,se_jpeg,1);strip=SE_NEW(&arena,uint8_t,(size_t)8*ow*3);
 if(!c.enc||!strip||se_look_begin(&c.look,&look,ow,&la)!=SE_OK){free(memory);goto done;}
 c.out=fopen(argv[3],"wb");if(!c.out){free(memory);goto done;}
 c.width=ow;c.base=off;c.total=len;c.hash=2166136261u;
 if(se_jpeg_begin(c.enc,ow,oh,88,strip,0,wr,&c)!=SE_OK)goto pipeline_done;
 if(se_jpeg_decode_stream_scaled(header+off,hdrlen,len,rd,&c,scale,&arena,rows,&c,&ji)!=SE_OK)goto pipeline_done;
 if(se_look_end(c.look,emit,&c)!=SE_OK||se_jpeg_end(c.enc)!=SE_OK)goto pipeline_done;
 printf("preview offset=%u length=%u coded=%dx%d output=%dx%d reads=%u hash=%08X scratch_peak=%zu/%u look_peak=%zu/%zu\n",off,len,pw,ph,ow,oh,c.reads,c.hash,arena.peak,SCRATCH,la.peak,ls);rc=0;
pipeline_done:fclose(c.out);free(memory);
done:fclose(c.in);free(header);return rc;
}
