#ifndef __NOTIFY_BOX_H_
#define __NOTIFY_BOX_H_

void NotifyBoxHide();
void NotifyBox(int timeout, char* fmt, ...) ;
void NotifyBoxAt(int x, int y, int timeout, char* fmt, ...) ;

#endif
