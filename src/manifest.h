#ifndef UPDATA_MANIFEST_H
#define UPDATA_MANIFEST_H

#include <windows.h>
#include "app.h"

/* 更新清单的拉取与差异比对：updata.exe 与 updata32/64.dll 共用这一份实现，
   保证两边"是否需要更新"的判断完全一致。 */

typedef void (*ManifestStatusFn)(const wchar_t *text);

void manifest_init(void);                            /* 初始化内部锁，进程内调用一次即可 */
void manifest_set_status_fn(ManifestStatusFn fn);    /* GUI 用它把进度显示到界面；DLL 不需要 */
void manifest_free_files(void);                      /* 释放 g_app.files */
void manifest_task_state(FileTask *t, int state, const wchar_t *err);

/* 同步拉取清单 + 与本地文件做 SHA-256 比对，结果写入 g_app；
   成功返回 0，失败返回 -1 并把原因写入 err */
int manifest_fetch(wchar_t *err, int errcap);

/* 统计需要更新的文件数与总字节数（写入 g_app.updateCount / bytesTotal） */
void manifest_count(void);

#endif
