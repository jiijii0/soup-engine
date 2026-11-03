/* decode (buffer mode) + optional look, FNV-hash decoded and graded rows */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "se.h"
#include "stock.h"
#include "look.h"
#include "jpeg_dec.h"
typedef struct {uint32_t hd,hg;se_look_run *look;} C;
static int emit(void*p,const uint8_t*r,int w){C*c=p;for(int i=0;i<w*3;i++){c->hg^=r[i];c->hg*=16777619u;}return 0;}
static int rows(void*p,const uint8_t*rgb,int n,int w){C*c=p;for(int i=0;i<n*w*3;i++){c->hd^=rgb[i];c->hd*=16777619u;}
 return c->look?(se_look_rows(c->look,rgb,n,emit,c)==SE_OK?0:-1):0;}
int main(int argc,char**argv){
 FILE*f=fopen(argv[1],"rb");fseek(f,0,2);long n=ftell(f);rewind(f);uint8_t*b=malloc(n);fread(b,1,n,f);fclose(f);
 int scale=atoi(argv[3]);se_jdec_info ji;se_jpeg_dec_info(b,n,&ji);int ow,oh;se_jpeg_scaled_size(ji.width,ji.height,scale,&ow,&oh);
 static char text[16384];f=fopen(argv[2],"rb");size_t g=fread(text,1,sizeof text,f);fclose(f);
 se_stock st;se_stock_init(&st);se_stock_parse(text,g,&st);se_look lk;se_look_from_stock(&st,scale,&lk);size_t ls=se_look_fit(&lk,ow,SE_LOOK_BUDGET);
 static uint8_t mem[16<<20];se_arena a,la;se_arena_init(&a,mem,8<<20);se_arena_init(&la,mem+(8<<20),ls);
 C c={2166136261u,2166136261u,0};if(se_look_begin(&c.look,&lk,ow,&la)!=SE_OK)return 3;
 int rc=se_jpeg_decode_scaled(b,n,scale,&a,rows,&c,&ji);if(rc==SE_OK)rc=se_look_end(c.look,emit,&c);
 printf("rc=%d %dx%d dec=%08X graded=%08X\n",rc,ow,oh,c.hd,c.hg);return rc;}
