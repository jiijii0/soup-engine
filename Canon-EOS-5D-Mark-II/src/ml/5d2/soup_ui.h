/* soup_ui.h - the upstream film picker on the 5D Mark II (soup_ui.c). GPL v2. */
#ifndef SOUP_UI_H
#define SOUP_UI_H

#define SOUP_UI_MAX 48             /* same as FILM_MAX in soup.c */

typedef struct {
    int busy;           /* frames queued or developing          */
    int pct;            /* current frame, 0..100                */
    int done, total;    /* this batch                           */
    int last_secs;      /* how long the last frame took         */
    const char *file;   /* the frame being developed            */
} soup_progress;

/* provided by soup.c */
void        soup_ui_get_progress(soup_progress *p);
int         soup_ui_take_event(void);  /* 1 finished, 2 failed, 0 none; clears */
void        soup_flm_path(int num, char *out, int n);  /* B:/FILM/xxxCANON/FLM_nnnn.JPG */
void        soup_flm_path_in(int folder, int num, char *out, int n);
int         soup_play_current(int *folder, int *num);  /* photo in playback; -1 unknown */
int         soup_play_learn(char *msg, int msglen);    /* one learning step; 1 = learned */
int         soup_ui_count(void);
const char *soup_ui_name(int i);
const char *soup_ui_desc(int i);
int         soup_ui_current(void);
void        soup_ui_choose(int i);      /* select stock i and turn Film on */
void        soup_iso_tick(void);        /* Film sets ISO: call often       */
int         soup_ui_film_on(void);
int         soup_ui_fav(int i);         /* 1 if film i is a favourite      */
int         soup_ui_toggle_fav(int i);  /* returns film i's new position   */

/* provided by soup_ui.c */
void         soup_ui_start(void);
void         soup_ui_open_from_menu(void);
unsigned int soup_ui_keypress(unsigned int key);

#endif
