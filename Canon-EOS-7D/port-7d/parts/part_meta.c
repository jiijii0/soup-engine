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
