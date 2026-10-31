/*
 * 手表扩展模块 - 界面层对外接口
 */
#ifndef __WATCH_UI_H__
#define __WATCH_UI_H__

/* 界面任务主体，由 main.c 挂到内核线程 */
void watch_ui_task(void);

#endif
