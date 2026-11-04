/** \file
 * Dialog test code
 * Based on http://code.google.com/p/400plus/source/browse/trunk/menu_developer.c
 */
#include "dryos.h"
#include "bmp.h"
#include "tasks.h"
#include "debug.h"
#include "menu.h"
#include "property.h"
#include "config.h"
#include "gui.h"
#include "lens.h"

static struct semaphore * notify_box_show_sem = 0;
static struct semaphore * notify_box_main_sem = 0;

static int notify_box_timeout = 0;
static int notify_box_stop_request = 0;
static int notify_box_dirty = 0;
static int notify_box_x = 50;
static int notify_box_y = 50;
static char notify_box_msg[100];
static char notify_box_msg_tmp[100];

static void NotifyBox_task(void* priv)
{
    TASK_LOOP
    {
        // wait until some other task asks for a notification
        int err = take_semaphore(notify_box_show_sem, 500);
        if (err)
            continue;
        
        if (!notify_box_timeout) 
            continue;
        
        // show notification for a while, then redraw to erase it
        notify_box_stop_request = 0;
        notify_box_dirty = 0;
        for ( ; notify_box_timeout > 0 ; notify_box_timeout -= 50)
        {
            if (notify_box_dirty)
            {
                /* clear old message */
                redraw();
                notify_box_dirty = 0;
            }
            bmp_printf(FONT_LARGE, notify_box_x, notify_box_y, "%s", notify_box_msg);
            msleep(50);
            if (notify_box_stop_request)
                break;
        }
        notify_box_timeout = 0;
        redraw();
    }
}

TASK_CREATE( "notifybox_task", NotifyBox_task, 0, 0x1b, 0x1000 );

void NotifyBoxHide()
{
    notify_box_stop_request = 1;
}

static void notify_box_v(int x, int y, int timeout, char* fmt, va_list ap)
{
    /* The drawn text is a finished string. bmp_printf must not parse it
     * again, or a trailing percent sign is eaten as a format marker. */
    take_semaphore(notify_box_main_sem, 0);

    vsnprintf( notify_box_msg_tmp, sizeof(notify_box_msg_tmp)-1, fmt, ap );
    notify_box_x = x;
    notify_box_y = y;
    
    if (notify_box_timeout && streq(notify_box_msg_tmp, notify_box_msg)) 
    {
        // same message: do not redraw, just re-set the timeout
        notify_box_timeout = MAX(timeout, 100);
        goto end;
    }

    // new message
    qprint("[NotifyBox] "); qprint(notify_box_msg_tmp); qprint("\n");
    memcpy(notify_box_msg, notify_box_msg_tmp, sizeof(notify_box_msg));
    notify_box_msg[sizeof(notify_box_msg)-1] = '\0';
    notify_box_timeout = MAX(timeout, 100);
    if (notify_box_timeout)
        notify_box_dirty = 1; // ask for a redraw, message changed

    give_semaphore(notify_box_show_sem); // request displaying the notification box

end:
    give_semaphore(notify_box_main_sem); // done, other call can be made now
}

void NotifyBox(int timeout, char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    notify_box_v(50, 50, timeout, fmt, ap);
    va_end(ap);
}

void NotifyBoxAt(int x, int y, int timeout, char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    notify_box_v(x, y, timeout, fmt, ap);
    va_end(ap);
}

static void dlg_init()
{
    notify_box_show_sem = create_named_semaphore("nbox_show_sem", SEM_CREATE_LOCKED);
    notify_box_main_sem = create_named_semaphore("nbox_done_sem", SEM_CREATE_UNLOCKED);
}

INIT_FUNC(__FILE__, dlg_init);
