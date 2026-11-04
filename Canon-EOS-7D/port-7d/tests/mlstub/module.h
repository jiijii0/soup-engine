/* Host stand-ins for the ML module API used by parts/head.c, part_frames.c,
 * part_meta.c and part_job.c, so the real develop runs on the Mac. Canon
 * FIO paths ("A:/DCIM/...", "ML/...") map into STUB_ROOT on disk. Tasks run
 * synchronously: task_create calls the task and returns. */
#ifndef SOUP_MLSTUB_H
#define SOUP_MLSTUB_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
extern char STUB_ROOT[256];
static inline void stub_path(const char *p,char *out){
    if(!strncmp(p,"A:/",3))p+=3;
    snprintf(out,512,"%s/%s",STUB_ROOT,p);
}
#define O_RDONLY 0
#define ATTR_DIRECTORY 0x10
#define IS_ERROR(p) ((uintptr_t)(p)&1)
#define CBR_RET_CONTINUE 0
struct fio_file{uint32_t mode,size,timestamp,off_0x0c;char name[128];};
struct fio_dirent{DIR *d;};
struct card_info{int file_number;};
struct menu_entry{int unused;};
struct lens_info_stub{unsigned job_state;};
extern struct lens_info_stub lens_info;
extern struct card_info stub_card;
extern int ml_shutdown_requested,lv,menu_redraw_blocked,stub_half;
static inline FILE *FIO_OpenFile(const char *p,int m){char q[512];(void)m;stub_path(p,q);return fopen(q,"rb");}
static inline FILE *FIO_CreateFile(const char *p){char q[512];stub_path(p,q);return fopen(q,"w+b");}
static inline void FIO_CloseFile(FILE *f){fclose(f);}
static inline int FIO_ReadFile(FILE *f,void *b,int n){return (int)fread(b,1,n,f);}
static inline int FIO_WriteFile(FILE *f,const void *b,int n){return (int)fwrite(b,1,n,f);}
static inline int64_t FIO_SeekSkipFile(FILE *f,int64_t off,int wh){return fseeko(f,off,wh)==0?ftello(f):-1;}
static inline int FIO_GetFileSize(const char *p,uint32_t *s){char q[512];struct stat st;stub_path(p,q);if(stat(q,&st))return -1;*s=(uint32_t)st.st_size;return 0;}
static inline int FIO_RemoveFile(const char *p){char q[512];stub_path(p,q);return unlink(q);}
/* Canon's rename is assumed to refuse an existing target; the code checks first */
static inline int FIO_RenameFile(const char *a,const char *b){char q[512],r[512];struct stat st;stub_path(a,q);stub_path(b,r);if(!stat(r,&st))return -1;return rename(q,r);}
static inline int FIO_CreateDirectory(const char *p){char q[512];stub_path(p,q);return mkdir(q,0755);}
static inline int is_file(const char *p){char q[512];struct stat st;stub_path(p,q);return !stat(q,&st)&&S_ISREG(st.st_mode);}
static inline int is_dir(const char *p){char q[512];struct stat st;stub_path(p,q);return !stat(q,&st)&&S_ISDIR(st.st_mode);}
static inline int stub_next(struct fio_dirent *d,struct fio_file *f){struct dirent *e;while((e=readdir(d->d))){memset(f,0,sizeof *f);snprintf(f->name,sizeof f->name,"%s",e->d_name);if(e->d_type==DT_DIR)f->mode=ATTR_DIRECTORY;return 0;}return -1;}
static inline struct fio_dirent *FIO_FindFirstEx(const char *p,struct fio_file *f){char q[512];static struct fio_dirent d;stub_path(p,q);d.d=opendir(q);if(!d.d)return (struct fio_dirent*)1;if(stub_next(&d,f))memset(f,0,sizeof *f);return &d;}
static inline int FIO_FindNextEx(struct fio_dirent *d,struct fio_file *f){return stub_next(d,f);}
static inline void FIO_FindClose(struct fio_dirent *d){closedir(d->d);}
static inline void *fio_malloc(size_t n){return malloc(n);}
static inline void fio_free(void *p){free(p);}
static inline void *tmp_malloc(size_t n){return malloc(n);}
static inline void msleep(int ms){(void)ms;}
static inline int task_create(const char *n,int pr,int st,void (*fn)(void*),void *a){(void)n;(void)pr;(void)st;fn(a);return 0;}
static inline void NotifyBox(int ms,const char *fmt,...){(void)ms;(void)fmt;}
static inline const char *get_dcim_dir(void){return "A:/DCIM/100EOS7D";}
static inline struct card_info *get_shooting_card(void){return &stub_card;}
static inline int get_ms_clock(void){struct timeval t;gettimeofday(&t,0);return (int)(t.tv_sec*1000+t.tv_usec/1000);}
static inline uint64_t get_us_clock(void){struct timeval t;gettimeofday(&t,0);return (uint64_t)t.tv_sec*1000000+t.tv_usec;}
static inline int GetFreeMemForAllocateMemory(void){return 0;}
static inline int GetFreeMemForMalloc(void){return 0;}
static inline int gui_menu_shown(void){return 0;}
static inline int is_pure_play_photo_or_movie_mode(void){return 0;}
extern int stub_half_after;   /* >0: report a half-press after this many polls */
static inline int get_halfshutter_pressed(void){if(stub_half_after>0&&--stub_half_after==0)stub_half=1;return stub_half;}
static inline void powersave_prohibit(void){}
static inline void powersave_permit(void){}
#endif
