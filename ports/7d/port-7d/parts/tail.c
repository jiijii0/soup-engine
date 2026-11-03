
/* ======================================================== ML Film menu ===
 * Fallback for everything the Soup screen does that matters most. */
static MENU_SELECT_FUNC(menu_develop_latest)
{
    const char *why;int i;
    (void)priv;(void)delta;
    ensure_loaded();
    if(busy){NotifyBox(3000,"Soup: busy");return;}
    if(index_build()!=0||(i=latest_index())<0){NotifyBox(3000,"Soup: no photos in this folder");return;}
    sel_buf[0]=frames[i].num;
    why=begin_job(sel_buf,1);
    if(why)NotifyBox(3000,"Soup: %s",why);
}
static void ensure_loaded(void)
{
    if(loaded)return;
    loaded=1;
    diag_len=0;diag_buf[0]=0;
    diag_mem("ensure_loaded begin");
    load_stocks();load_cfg();
    {char t[48];snprintf(t,sizeof(t),"stocks loaded=%d",stock_count);diag(t);}
    diag_mem("ensure_loaded end");
    diag_flush();
    if(!stock_count){stock_choices[0]="No ML/FILM stocks";snprintf(status_text,sizeof(status_text),"Film stocks missing");}
    if(selected_stock>=stock_count)selected_stock=0;
}
static MENU_SELECT_FUNC(stock_select)
{
    (void)priv;ensure_loaded();
    if(stock_count){stock_step(delta<0?-1:1);save_cfg();}
}
static MENU_UPDATE_FUNC(show_stock)
{
    (void)entry;
    if(!loaded)MENU_SET_VALUE("press to load");
    else MENU_SET_VALUE("%s%s",stock_count&&stock_fav[selected_stock]?"* ":"",stock_count?stock_names[selected_stock]:"none in ML/FILM");
}
static MENU_SELECT_FUNC(toggle_res){(void)priv;(void)delta;ensure_loaded();set_res=!set_res;save_cfg();}
static MENU_UPDATE_FUNC(show_res){(void)entry;MENU_SET_VALUE("%s",set_res==0?"Full":"Half");}
static MENU_SELECT_FUNC(cycle_mode){(void)priv;ensure_loaded();set_mode=(set_mode+(delta<0?-1:1)+M_COUNT)%M_COUNT;save_cfg();}
static MENU_UPDATE_FUNC(show_mode){(void)entry;MENU_SET_VALUE("%s",mode_names[set_mode]);}
static MENU_SELECT_FUNC(toggle_auto){(void)priv;(void)delta;ensure_loaded();set_auto=!set_auto;save_cfg();}
static MENU_UPDATE_FUNC(show_auto){(void)entry;MENU_SET_VALUE("%s",set_auto?"ON":"OFF");}
static MENU_UPDATE_FUNC(show_status){(void)entry;MENU_SET_VALUE("%s",status_text);}
static struct menu_entry soup_menu[]={
    {.name="Film stock",.select=stock_select,.update=show_stock,
     .help="Soup Engine look for the next development."},
    {.name="Develop latest",.select=menu_develop_latest,
     .help="Or press Picture Style (outside LiveView) for the Soup screen."},
    {.name="Output size",.select=toggle_res,.update=show_res,.help="Full takes about four times as long as half."},
    {.name="Output",.select=cycle_mode,.update=show_mode,
     .help="JPEG ONLY: the developed JPEG replaces Canon's JPEG and the RAW.",
     .help2="REPLACE JPEG keeps the RAW. NEW JPEG modes keep the originals."},
    {.name="Auto develop",.select=toggle_auto,.update=show_auto,
     .help="Develops automatically when you stop shooting."},
    {.name="Soup status",.update=show_status,.help="Experimental 7D 2.0.3 module."},
};
static unsigned int soup_init(void)
{
    if(!is_camera("7D","2.0.3"))return 1;
    /* no card I/O here -- see ensure_loaded() */
    menu_add("Film",soup_menu,COUNT(soup_menu));return 0;
}
static unsigned int soup_deinit(void)
{
    stop_worker=1;
    /* Shutdown must not return while our tasks are using the card/module. */
    menu_remove("Film",soup_menu,COUNT(soup_menu));
    while(busy||gui_task_alive)msleep(20);
    menu_redraw_blocked=0;
    if(frames){free(frames);frames=0;}
    return 0;
}
MODULE_INFO_START()
    MODULE_INIT(soup_init)
    MODULE_DEINIT(soup_deinit)
    MODULE_LONGNAME("Soup Engine 7D prototype")
MODULE_INFO_END()
MODULE_CBRS_START()
    MODULE_CBR(CBR_KEYPRESS, soup_keypress, 0)
    MODULE_CBR(CBR_SECONDS_CLOCK, soup_clock, 0)
MODULE_CBRS_END()
