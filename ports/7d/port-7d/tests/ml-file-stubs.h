/* In-memory file stubs for host tests of part_frames.c (numbering, EXIF
 * source lookup, self-heal rename). Paths are full ML-style paths. */
typedef struct { char path[96]; const char *data; uint32_t len; } memfile;
static memfile mfs[64]; static int nmfs;
typedef struct { int idx; } FILE_STUB;
static FILE_STUB fhs[8];
static void diag(const char *s){ printf("  diag: %s\n",s); }
static void diag_flush(void){}
static int good_file(FILE *f){ return f!=0; }
static memfile *mf_find(const char *p){ for(int i=0;i<nmfs;i++) if(!strcmp(mfs[i].path,p)) return &mfs[i]; return 0; }
static void mf_add(const char *p,const char *data){ memfile *m=&mfs[nmfs++]; snprintf(m->path,sizeof m->path,"%s",p); m->data=data; m->len=data?strlen(data):64; }
static int is_file(const char *p){ return mf_find(p)!=0; }
static int FIO_GetFileSize(const char *p,uint32_t *s){ memfile *m=mf_find(p); if(!m)return -1; *s=m->len<64?64:m->len; return 0; }
static FILE *FIO_OpenFile(const char *p,int mode){ (void)mode; memfile *m=mf_find(p); if(!m)return 0; fhs[0].idx=(int)(m-mfs); return (FILE*)&fhs[0]; }
static void FIO_CloseFile(FILE *f){ (void)f; }
static int read_exact(FILE *f,uint32_t off,void *dst,uint32_t n){
  memfile *m=&mfs[((FILE_STUB*)f)->idx]; memset(dst,0,n);
  if(m->data){ uint32_t l=m->len; if(off<l) memcpy(dst,m->data+off,(n<l-off)?n:l-off); }
  return 0; }
static int renames;
static int FIO_RenameFile(const char *a,const char *b){ memfile *m=mf_find(a); if(!m||mf_find(b))return -1; snprintf(m->path,sizeof m->path,"%s",b); renames++; return 0; }
static int removes;
static int FIO_RemoveFile(const char *p){ memfile *m=mf_find(p); if(!m)return -1; *m=mfs[--nmfs]; removes++; return 0; }
static void *fio_malloc(size_t n){ return malloc(n); }
static void fio_free(void *p){ free(p); }
#define O_RDONLY 0
