/* host: decode a 4:2:2 JPEG, keep a centred W x H crop, re-encode it 4:2:2 (Canon's layout) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "se.h"
#include "jpeg_dec.h"
#include "jpeg_enc.h"
enum{W=640,H=432};
static uint8_t crop[W*H*3];static int y0_,x0_,yy;
static int sink(void*c,const uint8_t*rgb,int n,int w){for(int i=0;i<n;i++,yy++){int r=yy-y0_;if(r>=0&&r<H)memcpy(crop+r*W*3,rgb+i*w*3+x0_*3,W*3);}return 0;}
static int wr(void*p,const uint8_t*d,size_t n){return fwrite(d,1,n,p)==n?0:-1;}
int main(int c,char**v){FILE*f=fopen(v[1],"rb");fseek(f,0,2);long n=ftell(f);rewind(f);uint8_t*b=malloc(n);fread(b,1,n,f);fclose(f);
 se_jdec_info ji;se_jpeg_dec_info(b,n,&ji);y0_=(ji.height-H)/2;x0_=(ji.width-W)/2&~15;
 static uint8_t mem[64<<20];se_arena a;se_arena_init(&a,mem,sizeof mem);
 if(se_jpeg_decode_scaled(b,n,1,&a,sink,0,&ji)!=SE_OK)return 1;
 se_jpeg *j=malloc(sizeof *j);uint8_t*strip=malloc(8*W*3);FILE*o=fopen(v[2],"wb");
 se_jpeg_begin(j,W,H,97,strip,0,wr,o);se_jpeg_write_rows(j,crop,H);se_jpeg_end(j);fclose(o);return 0;}
