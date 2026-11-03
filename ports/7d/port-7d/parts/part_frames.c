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
