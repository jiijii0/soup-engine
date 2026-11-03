/* ===================================================== Soup screen ====
 * Picture Style (outside Live View, playback and ML's menu) opens a
 * full-screen Soup view. Same mechanism as ML's own arkanoid module: open
 * ML's menu so Canon's GUI is suspended, block ML's menu redraw, and paint
 * the whole overlay from our own task. Buttons arrive through the standard
 * module CBR_KEYPRESS hook. No ROM addresses, palette registers or GUI-task
 * patches -- unlike the 5D payload, which needs all three.
 *
 * The engine UI renders a 720x240 4-bit buffer (the 5D's native overlay);
 * the 7D overlay is 720x480 8-bit, so rows are doubled and the 16 UI
 * colours are mapped onto ML's standard palette. Picker thumbnails are
 * drawn straight into the 8-bit overlay in ML's grey ramp: the palette has
 * no room for colour previews.
 */
enum {G_STOCK, G_SETTINGS, G_DEVMENU, G_PICK, G_WORK, G_DONE};
enum {REQ_NONE, REQ_DEVMENU, REQ_LATEST, REQ_NEW, REQ_LASTN, REQ_PICKONE};
static se_ui gui;
static se_ui_stock gui_stocks[MAX_STOCKS];
static volatile int gui_dirty, gui_close, gui_req, gui_save, gui_restock;
static volatile int gmode, gsetting, gdev, pick_i;
static char gui_note[64], gui_info[64], gui_v[4][24], gui_lasthelp[32], gui_right[24], gui_title[24];
static int dev_latest=-1, dev_new;
static const int lastn_choices[5]={2,3,5,10,20};
static uint16_t sel_buf[MAX_JOB];

/* picker state, owned by the GUI task */
static uint8_t *pk_hdr, *pk_arena, *pk_gray;
static int pk_loaded=-2, pk_ok, pk_w, pk_h, pk_orient;

static const uint8_t gui_colours[16]={
    COLOR_BLACK,        /* CLEAR  (opaque here: nothing should show through) */
    COLOR_GRAY(6),      /* INK    */
    COLOR_GRAY(22),     /* SHADOW */
    COLOR_GRAY(38),     /* SLATE  */
    COLOR_GRAY(55),     /* MID    */
    COLOR_GRAY(72),     /* EDGE   */
    COLOR_GRAY(88),     /* PAPER  */
    COLOR_GRAY(96),     /* HILITE */
    COLOR_DARK_RED,     /* RUST   */
    COLOR_RED,          /* RED    */
    COLOR_ORANGE,       /* AMBER  */
    COLOR_YELLOW,       /* GOLD   */
    COLOR_GREEN2,       /* OLIVE  */
    COLOR_CYAN,         /* TEAL   */
    COLOR_MAGENTA,      /* PLUM   */
    COLOR_WHITE,        /* WHITE  */
};

static void gui_blit(const uint8_t *fb)
{
    uint8_t *v=bmp_vram();int x,y;
    if(!v)return;
    for(y=0;y<SE_UI_H;y++){
        const uint8_t *src=fb+y*SE_UI_PITCH;
        uint8_t *d0=v+(2*y)*BMPPITCH,*d1=d0+BMPPITCH;
        for(x=0;x<SE_UI_PITCH;x++){
            uint8_t b=src[x],lo=gui_colours[b&15],hi=gui_colours[b>>4];
            /* even x in the low nibble, odd x in the high one (ui.c) */
            d0[2*x]=lo;d0[2*x+1]=hi;d1[2*x]=lo;d1[2*x+1]=hi;
        }
    }
}

/* Thumbnail into a box of the 8-bit overlay, rotated per EXIF and scaled
 * (nearest) to fit, in ML's 42-step grey ramp. */
#define PK_BX 200
#define PK_BY 68
#define PK_BW 320
#define PK_BH 240
static void pick_blit_thumb(const uint8_t *gray,int w,int h,int orient)
{
    uint8_t *v=bmp_vram();int dw,dh,ox,oy,dx,dy,sx,sy,num,den;
    if(!v||w<1||h<1)return;
    if(orient>=5){dw=h;dh=w;}else{dw=w;dh=h;}
    /* scale = num/den, fitting dw x dh into the box */
    if((long)PK_BW*dh<=(long)PK_BH*dw){num=PK_BW;den=dw;}else{num=PK_BH;den=dh;}
    ox=PK_BX+(PK_BW-dw*num/den)/2;oy=PK_BY+(PK_BH-dh*num/den)/2;
    for(dy=0;dy<dh*num/den;dy++){
        uint8_t *row=v+(oy+dy)*BMPPITCH+ox;
        int ry=dy*den/num;
        for(dx=0;dx<dw*num/den;dx++){
            int rx=dx*den/num;
            switch(orient){
            case 3: sx=w-1-rx;sy=h-1-ry;break;
            case 6: sx=ry;sy=h-1-rx;break;
            case 8: sx=w-1-ry;sy=rx;break;
            default:sx=rx;sy=ry;break;
            }
            row[dx]=(uint8_t)COLOR_GRAY(gray[sy*THUMB_W+sx]*100/255);
        }
    }
}

static void pick_free(void)
{
    if(pk_hdr){fio_free(pk_hdr);pk_hdr=0;}
    if(pk_arena){free(pk_arena);pk_arena=0;}
    if(pk_gray){free(pk_gray);pk_gray=0;}
    pk_loaded=-2;
}
/* Load the thumbnail of frames[i]. Prefers the JPG (EXIF is in its first
 * 64 KB); a CR2's thumbnail sits about 57 KB in on the 7D. */
static int pk_logged;
static void pick_load(int i)
{
    frame_ent *e;char path[96];FILE *f;uint32_t size=0,n;meta_info m;int jpg;
    pk_loaded=i;pk_ok=0;
    if(i<0||i>=nframes)return;
    if(!pk_hdr)pk_hdr=fio_malloc(HDR_CAP);
    if(!pk_arena)pk_arena=tmp_malloc(64*1024);
    if(!pk_gray)pk_gray=malloc(THUMB_W*THUMB_H);
    if(!pk_hdr||!pk_arena||!pk_gray){
        static int said;char t[96];
        if(!said){said=1;snprintf(t,sizeof(t),"pick alloc failed hdr=%d arena=%d gray=%d",!!pk_hdr,!!pk_arena,!!pk_gray);
            diag(t);diag_mem("pick");diag_flush();}
        return;}
    e=&frames[i];jpg=(e->flags&F_JPG)!=0;
    frame_path(e,!jpg,path,sizeof(path));
#define PK_FAIL(step,v) do{if(pk_logged<4){char t_[128];pk_logged++;snprintf(t_,sizeof(t_),"pick %s: %s (%d)",path,step,(int)(v));diag(t_);diag_flush();}return;}while(0)
    if(FIO_GetFileSize(path,&size)!=0)PK_FAIL("getsize",-1);
    if(size<64)PK_FAIL("too small",size);
    n=jpg?(size<64*1024?size:64*1024):(size<HDR_CAP?size:HDR_CAP);
    f=FIO_OpenFile(path,O_RDONLY);if(!good_file(f))PK_FAIL("open",(int)f);
    if(read_exact(f,0,pk_hdr,n)!=0){FIO_CloseFile(f);PK_FAIL("read n",n);}
    FIO_CloseFile(f);
    if(jpg&&has_tag(pk_hdr,n))e->done=1;    /* developed in place */
    read_meta(pk_hdr,n,!jpg,&m);
    if(!m.thumb_len)PK_FAIL("no exif thumb, head",(pk_hdr[0]<<8)|pk_hdr[1]);
    if(decode_thumb_gray(pk_hdr+m.thumb_off,m.thumb_len,pk_arena,64*1024,pk_gray,&pk_w,&pk_h)!=0)PK_FAIL("thumb decode len",m.thumb_len);
#undef PK_FAIL
    pk_orient=m.orient;pk_ok=1;
}

static void draw_picker(se_ui_fb *fb)
{
    char a[40],b[48];frame_ent *e;int x;
    if(nframes<1||pick_i<0||pick_i>=nframes){
        se_ui_chrome(fb,"PICK A FRAME",0,"[MENU] BACK");
        se_ui_text_c(fb,SE_UI_W/2,100,"NO PHOTOS IN THIS FOLDER",SE_C_RED,3,2);
        return;
    }
    e=&frames[pick_i];
    snprintf(a,sizeof(a),"%d OF %d",pick_i+1,nframes);
    se_ui_chrome(fb,"PICK A FRAME",a,"[\x01\x02] FRAME   [TOP DIAL] 10   [SET] DEVELOP   [MENU] BACK");
    /* the thumbnail box; the picture itself goes straight to the overlay */
    se_ui_fill(fb,PK_BX-2,PK_BY/2-1,PK_BW+4,PK_BH/2+2,SE_C_INK);
    se_ui_fill(fb,PK_BX,PK_BY/2,PK_BW,PK_BH/2,SE_C_SHADOW);
    if(pk_loaded!=pick_i)se_ui_text_c(fb,SE_UI_W/2,PK_BY/2+PK_BH/4-4,"LOADING",SE_C_EDGE,2,1);
    else if(!pk_ok)se_ui_text_c(fb,SE_UI_W/2,PK_BY/2+PK_BH/4-4,"NO PREVIEW",SE_C_EDGE,2,1);
    if(pick_i>0)se_ui_text(fb,150,86,SE_SYM_LEFT,SE_C_MID,3,2);
    if(pick_i<nframes-1)se_ui_text(fb,SE_UI_W-170,86,SE_SYM_RIGHT,SE_C_MID,3,2);
    snprintf(a,sizeof(a),"#%04d",e->num);
    se_ui_text_c(fb,SE_UI_W/2,160,a,SE_C_INK,3,2);
    snprintf(b,sizeof(b),"%s%s%s",(e->flags&F_CR2)?"CR2":"",(e->flags&F_CR2)&&(e->flags&F_JPG)?" + ":"",(e->flags&F_JPG)?"JPG":"");
    if(e->flags&F_DUP)snprintf(b,sizeof(b),"SEVERAL FILES - SKIPPED");
    x=se_ui_text_w(b,2);
    if(e->done){
        int w2=se_ui_text_w("   DEVELOPED",2);
        se_ui_text(fb,(SE_UI_W-x-w2)/2,184,b,SE_C_SLATE,2,1);
        se_ui_text(fb,(SE_UI_W-x-w2)/2+x,184,"   DEVELOPED",SE_C_OLIVE,2,1);
    }else se_ui_text_c(fb,SE_UI_W/2,184,b,SE_C_SLATE,2,1);
}

/* Settings (INFO) and the develop menu (SET) share the engine's settings
 * screen with port-supplied rows. */
static void gui_prepare(void)
{
    gui.stock=stock_count?stock_pos(selected_stock):0;
    snprintf(gui_info,sizeof(gui_info),"%s / %s%s",set_res==0?"FULL":"HALF",mode_names[set_mode],set_auto?" / AUTO":"");
    gui.info_label="OUTPUT: ";gui.info_value=gui_info;
    gui.info_colour=0;
    gui.value[SE_SET_SOURCE]=SE_SOURCE_PREVIEW;gui.value[SE_SET_RES]=set_res;
    if(gmode==G_SETTINGS){
        gui.screen=SE_SCREEN_SETTINGS;gui.set_n=3;gui.setting=gsetting;
        gui.settings_title=0;gui.settings_right=0;
        gui.set_label[0]="SIZE";      gui.set_value[0]=set_res==0?"FULL  5184 PX":"HALF  2592 PX";
        gui.set_help[0]="FULL TAKES ABOUT FOUR TIMES AS LONG";
        {static const char *const mode_help[M_COUNT]={
            "THE DEVELOPED JPEG REPLACES THE CAMERA JPEG AND RAW",
            "KEEPS THE RAW BUT PLAYBACK SHOWS THE RAW PREVIEW",
            "SAVED NEXT TO THE ORIGINALS UNDER A NEW NUMBER",
            "DELETES THE RAW ONLY IF A FULL-SIZE JPEG IS KEPT",
            "NEW JPEG IN /ML/SOUP FOLDER"};
        gui.set_label[1]="OUTPUT";gui.set_value[1]=mode_names[set_mode];gui.set_help[1]=mode_help[set_mode];}
        gui.set_label[2]="AUTO DEVELOP";gui.set_value[2]=set_auto?"ON":"OFF";
        gui.set_help[2]=set_auto?"DEVELOPS AUTOMATICALLY WHEN YOU STOP SHOOTING":"DEVELOP BY HAND WITH SET ON THE FILM SCREEN";
        gui.settings_hint="[DIAL] ROW   [\x01\x02] CHANGE   [SET] BACK";
    }else if(gmode==G_DEVMENU){
        gui.screen=SE_SCREEN_SETTINGS;gui.set_n=4;gui.setting=gdev;
        snprintf(gui_title,sizeof(gui_title),"DEVELOP");gui.settings_title=gui_title;
        snprintf(gui_right,sizeof(gui_right),"%s",stock_count?stock_names[selected_stock]:"");gui.settings_right=gui_right;
        snprintf(gui_v[0],24,"%d FRAMES",nframes);
        if(dev_latest>=0)snprintf(gui_v[1],24,"#%04d",frames[dev_latest].num);else snprintf(gui_v[1],24,"NONE");
        if(dev_new)snprintf(gui_v[2],24,"%d FRAME%s",dev_new,dev_new==1?"":"S");else snprintf(gui_v[2],24,"NONE");
        snprintf(gui_v[3],24,"\x01 %d \x02",lastn_choices[set_lastn]);
        gui.set_label[0]="PICK";    gui.set_value[0]=gui_v[0];gui.set_help[0]="CHOOSE PICTURE TO DEVELOP";
        gui.set_label[1]="LATEST";  gui.set_value[1]=gui_v[1];gui.set_help[1]="THE LAST PHOTO TAKEN";
        gui.set_label[2]="ALL NEW"; gui.set_value[2]=gui_v[2];gui.set_help[2]="EVERYTHING SINCE THE LAST DEVELOP";
        snprintf(gui_lasthelp,sizeof(gui_lasthelp),"THE LAST " SE_ITALIC "%d" SE_ITALIC " PHOTOS",lastn_choices[set_lastn]);
        gui.set_label[3]="LAST";    gui.set_value[3]=gui_v[3];gui.set_help[3]=gui_lasthelp;
        gui.settings_hint=gdev==3?"[DIAL] ROW   [\x01\x02] HOW MANY   [SET] GO   [MENU] BACK":"[DIAL] ROW   [SET] GO   [MENU] BACK";
    }else if(gmode==G_WORK){
        gui.screen=SE_SCREEN_WORKING;
        gui.percent=progress_pct;gui.stage=cancel_job?"CANCELLING":"DEVELOPING";
        gui.done=job_pos;gui.total=job_total;
        snprintf(gui.file,sizeof(gui.file),"#%04d",job_nums[job_pos<job_total?job_pos:0]);
    }else if(gmode==G_DONE){
        gui.screen=SE_SCREEN_DONE;
    }else gui.screen=SE_SCREEN_STOCK;
}

static void gui_finish_job(void)
{
    gui.done=job_ok;gui.last_seconds=(job_ms+500)/1000;
    gui.done_colour=0;gui.done_title="DEVELOPED";
    /* Canon's playback list only gains files Canon writes itself, or finds
     * when it re-catalogues at power-on: ours appear after a restart. So
     * does a JPEG ONLY frame (Canon still has it as a RAW+JPEG pair).
     * REPLACE JPEG frames never look developed in playback: the CR2 is shown. */
    if(job_ok&&job_total==1)
        snprintf(gui_note,sizeof(gui_note),"%s %s%s%s",job_mode<=M_INPLACE?"REPLACED":"SAVED",
            strrchr(last_out,'/')?strrchr(last_out,'/')+1:last_out,
            job_deleted&&job_mode!=M_ONE?", RAW DELETED":"",job_mode==M_INPLACE||job_out?"":" - RESTART CAMERA TO VIEW");
    else if(job_ok&&!job_failed)
        snprintf(gui_note,sizeof(gui_note),"%s%s",job_mode==M_ONE?"JPEGS REPLACED - RESTART CAMERA TO VIEW":
            job_mode==M_INPLACE?"JPEGS REPLACED":
            job_out?"SAVED TO ML/SOUP":"RESTART CAMERA TO VIEW IN PLAYBACK",job_deleted&&job_mode!=M_ONE?", RAWS DELETED":"");
    else if(job_ok){gui.done_title="PARTLY DONE";gui.done_colour=SE_C_AMBER;
        snprintf(gui_note,sizeof(gui_note),"%d FAILED, %d SKIPPED - SEE ML/SOUP/FIND.TXT",job_failed,job_skipped);}
    else if(cancel_job){gui.done_title="CANCELLED";gui.done_colour=SE_C_MID;snprintf(gui_note,sizeof(gui_note),"ORIGINALS UNCHANGED");}
    else if(job_result==-2){gui.done_title="NOTHING DONE";gui.done_colour=SE_C_RED;snprintf(gui_note,sizeof(gui_note),"FRAMES SKIPPED - SEE ML/SOUP/FIND.TXT");}
    else{gui.done_title="FAILED";gui.done_colour=SE_C_RED;snprintf(gui_note,sizeof(gui_note),"ORIGINALS UNCHANGED - SEE ML/SOUP/FIND.TXT");}
    gui.done_note=gui_note;
    gmode=G_DONE;
}

static void gui_run(int count)
{
    const char *why=begin_job(sel_buf,count);
    if(why){gui.done=0;gui.last_seconds=0;gui.done_title="NOT STARTED";gui.done_colour=SE_C_RED;
        snprintf(gui_note,sizeof(gui_note),"%s",why);gui.done_note=gui_note;gmode=G_DONE;}
    else gmode=G_WORK;
}

/* Everything that touches the card happens here, never in the key handler. */
static void gui_handle_request(void)
{
    int r=gui_req;gui_req=REQ_NONE;
    switch(r){
    case REQ_DEVMENU:
        if(index_build()!=0||nframes<1){gui.done=0;gui.last_seconds=0;gui.done_title="NO PHOTOS";gui.done_colour=SE_C_RED;
            snprintf(gui_note,sizeof(gui_note),"NOTHING IN %s",frames_dir);gui.done_note=gui_note;gmode=G_DONE;break;}
        dev_latest=latest_index();dev_new=select_new(sel_buf,MAX_JOB);
        gdev=0;gmode=G_DEVMENU;break;
    case REQ_LATEST: if(dev_latest>=0){sel_buf[0]=frames[dev_latest].num;gui_run(1);}break;
    case REQ_NEW:    gui_run(select_new(sel_buf,MAX_JOB));break;
    case REQ_LASTN:  gui_run(select_last(sel_buf,MAX_JOB,lastn_choices[set_lastn]));break;
    case REQ_PICKONE:if(pick_i>=0&&pick_i<nframes){sel_buf[0]=frames[pick_i].num;pick_free();gui_run(1);}break;
    }
    gui_dirty=1;
}

/* The engine's list, in dial order: favourites first. */
static void gui_fill_stocks(void)
{
    int i;
    for(i=0;i<stock_count;i++){
        int k=stock_ord[i];
        gui_stocks[i].name=stock_names[k];gui_stocks[i].desc=stock_descs[k];gui_stocks[i].fav=stock_fav[k];
    }
    se_ui_set_stocks(&gui,gui_stocks,stock_count);
    gui.stock=stock_count?stock_pos(selected_stock):0;
}

static void gui_task(void *unused)
{
    uint8_t *fb=0;se_ui_fb f;int waited,last=0,done_at=0;
    (void)unused;
    fb=tmp_malloc(SE_UI_FB_SIZE);   /* off Canon heap: see Err70 notes */
    if(!fb)goto out;
    f.px=fb;
    for(waited=0;!gui_menu_shown()&&waited<50;waited++)msleep(20);
    if(!loaded){
        gui.ready=0;gui_prepare();se_ui_draw(&gui,&f);gui_blit(fb);   /* LOADING */
        ensure_loaded();
    }
    gui_fill_stocks();gui.ready=1;
    gui_dirty=1;
    while(!gui_close&&gui_menu_shown()&&!ml_shutdown_requested&&!stop_worker){
        if(gui_restock){gui_restock=0;gui_fill_stocks();}
        /* opened by auto develop: the shutter takes priority (the job stops on
         * the same half-press), and the result shows for a moment, then goes */
        if(gui_auto){
            if(get_halfshutter_pressed()||lv)break;   /* Live View came back */
            if(gmode==G_DONE){if(!done_at)done_at=get_ms_clock();else if(get_ms_clock()-done_at>2500)break;}
        }
        if(gui_save){gui_save=0;save_cfg();}
        if(gui_req)gui_handle_request();
        if(busy&&gmode!=G_WORK){gmode=G_WORK;gui_dirty=1;}
        if(!busy&&gmode==G_WORK){gui_finish_job();gui_dirty=1;}
        if(gmode==G_PICK&&pk_loaded!=pick_i){
            draw_picker(&f);gui_blit(fb);           /* shows LOADING meanwhile */
            pick_load(pick_i);gui_dirty=1;          /* keys may move on; reloads next pass */
        }
        if(gmode!=G_PICK&&pk_loaded!=-2)pick_free();
        if(gui_dirty||(busy&&get_ms_clock()-last>300)){
            gui_dirty=0;
            if(gmode==G_PICK){
                draw_picker(&f);gui_blit(fb);
                if(pk_loaded==pick_i&&pk_ok)pick_blit_thumb(pk_gray,pk_w,pk_h,pk_orient);
            }else{gui_prepare();se_ui_draw(&gui,&f);gui_blit(fb);}
            last=get_ms_clock();
        }
        msleep(40);
    }
out:
    if(gui_save){gui_save=0;save_cfg();}
    pick_free();
    gui_running=0;gui_auto=0;
    if(gui_menu_shown()){
        clrscr();gui_stop_menu();
        for(waited=0;gui_menu_shown()&&waited<50;waited++)msleep(20);
    }
    menu_redraw_blocked=0;
    if(fb)free(fb);
    if(!busy)frames_release();      /* a running job still uses it; it frees it */
    gui_task_alive=0;
}

static int gui_open(void)
{
    if(gui_task_alive)return -1;
    se_ui_init(&gui);
    se_ui_set_stocks(&gui,gui_stocks,0);   /* filled by the GUI task */
    gui.ready=loaded;gui.stock=0;
    gui.stock_hint="[\x01\x02] FILM  [SET] DEVELOP  [Q] FAV  [INFO] SETUP  [MENU] CLOSE";
    gmode=busy?G_WORK:G_STOCK;gsetting=0;gdev=0;pick_i=0;
    gui_close=0;gui_req=REQ_NONE;gui_save=0;gui_dirty=1;pk_loaded=-2;
    gui_task_alive=1;gui_running=1;
    menu_redraw_blocked=1;           /* ML's menu must never paint over us */
    if(IS_ERROR(task_create("soup7d_gui",0x1c,0x4000,gui_task,0))){
        gui_running=0;gui_task_alive=0;menu_redraw_blocked=0;return -1;
    }
    /* ML paints audio meters over any open menu whose selected tab is
     * "Audio"; park its selection on our own Film tab first. */
    select_menu("Film",0);
    gui_open_menu();
    return 0;
}

/* dir: -1/+1 for joystick or rear dial, -10/+10 for the top dial */
static void gui_turn(int dir)
{
    int step=dir>0?1:-1;
    switch(gmode){
    case G_STOCK:
        if(stock_count){stock_step(step);gui_save=1;}
        break;
    case G_SETTINGS:
        if(gsetting==0)set_res=!set_res;
        else if(gsetting==1)set_mode=(set_mode+step+M_COUNT)%M_COUNT;
        else set_auto=!set_auto;
        break;
    case G_DEVMENU:
        if(gdev==3){int n=set_lastn+step;if(n>=0&&n<5)set_lastn=n;gui_save=1;}
        break;
    case G_PICK:
        {int n=pick_i+dir;if(n<0)n=0;if(n>=nframes)n=nframes-1;pick_i=n;}
        break;
    case G_DONE:
        gmode=G_STOCK;break;
    }
}
static void gui_row(int step)
{
    if(gmode==G_SETTINGS)gsetting=(gsetting+step+3)%3;
    else if(gmode==G_DEVMENU)gdev=(gdev+step+4)%4;
    else if(gmode==G_PICK)gui_turn(step);
}
static void gui_set(void)
{
    switch(gmode){
    case G_STOCK:   if(!busy&&!gui_req)gui_req=REQ_DEVMENU;break;
    case G_SETTINGS:gmode=G_STOCK;gui_save=1;break;
    case G_DEVMENU:
        if(gui_req)break;
        if(gdev==0){gmode=G_PICK;pick_i=dev_latest>=0?dev_latest:nframes-1;}
        else if(gdev==1)gui_req=REQ_LATEST;else if(gdev==2)gui_req=REQ_NEW;else gui_req=REQ_LASTN;
        break;
    case G_PICK:    if(!gui_req)gui_req=REQ_PICKONE;break;
    case G_DONE:    gmode=G_STOCK;break;
    }
}
static void gui_back(void)
{
    switch(gmode){
    case G_WORK:    cancel_job=1;break;           /* MENU cancels a running job */
    case G_SETTINGS:gmode=G_STOCK;gui_save=1;break;
    case G_DEVMENU: gmode=G_STOCK;break;
    case G_PICK:    gmode=G_DEVMENU;break;
    case G_DONE:    gmode=G_STOCK;break;
    default:        gui_close=1;break;
    }
}

static unsigned int soup_keypress(unsigned int key)
{
    if(!gui_running){
        if(key!=MODULE_KEY_PICSTYLE)return 1;
        /* leave Live View's arrow shortcut and playback zebras to ML */
        if(gui_menu_shown()||lv||is_pure_play_photo_or_movie_mode()||ml_shutdown_requested)return 1;
        gui_auto=0;
        return gui_open()==0?0:1;
    }
    switch(key){
    /* 7D: rear dial = WHEEL_UP/DOWN, top dial = WHEEL_LEFT/RIGHT */
    case MODULE_KEY_PRESS_LEFT: gui_turn(-1);break;
    case MODULE_KEY_PRESS_RIGHT:gui_turn(1);break;
    case MODULE_KEY_WHEEL_UP:   if(gmode==G_DEVMENU||gmode==G_SETTINGS)gui_row(-1);else gui_turn(-1);break;
    case MODULE_KEY_WHEEL_DOWN: if(gmode==G_DEVMENU||gmode==G_SETTINGS)gui_row(1);else gui_turn(1);break;
    case MODULE_KEY_WHEEL_LEFT: gui_turn(gmode==G_PICK?-10:-1);break;
    case MODULE_KEY_WHEEL_RIGHT:gui_turn(gmode==G_PICK?10:1);break;
    case MODULE_KEY_PRESS_UP:   gui_row(-1);break;
    case MODULE_KEY_PRESS_DOWN: gui_row(1);break;
    case MODULE_KEY_PRESS_SET:case MODULE_KEY_JOY_CENTER: gui_set();break;
    case MODULE_KEY_INFO:
        if(gmode==G_STOCK&&!busy){gmode=G_SETTINGS;gsetting=0;}
        else if(gmode==G_SETTINGS){gmode=G_STOCK;gui_save=1;}
        break;
    case MODULE_KEY_MENU: gui_back();break;
    case MODULE_KEY_Q:
        /* favourite on/off (the 7D has no RATE button); the stock stays
         * selected and moves with its group */
        if(gmode==G_STOCK&&stock_count&&!gui_restock){
            stock_fav[selected_stock]=!stock_fav[selected_stock];
            order_build();gui_restock=1;gui_save=1;
        }
        break;
    case MODULE_KEY_PICSTYLE:case MODULE_KEY_TRASH:
        if(gmode==G_SETTINGS)gui_save=1;
        gui_close=1;break;                        /* a running job carries on */
    /* swallow the rest of the buttons so the hidden ML menu never acts */
    case MODULE_KEY_PRESS_UP_RIGHT:case MODULE_KEY_PRESS_UP_LEFT:
    case MODULE_KEY_PRESS_DOWN_RIGHT:case MODULE_KEY_PRESS_DOWN_LEFT:
    case MODULE_KEY_UNPRESS_UDLR:case MODULE_KEY_UNPRESS_SET:
    case MODULE_KEY_PLAY:case MODULE_KEY_PRESS_ZOOMIN:
    case MODULE_KEY_RATE:case MODULE_KEY_LV:
        break;
    default:
        return 1;   /* shutter, internal events: pass through untouched */
    }
    gui_auto=0;     /* a button was used: the screen stays, like any other time */
    gui_dirty=1;
    return 0;
}
