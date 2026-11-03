/* GENERATED from ../parts/part_job.c (TH_CAP..graded_thumb) */
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
    mem=malloc(160*1024);if(!mem)return 0;
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

