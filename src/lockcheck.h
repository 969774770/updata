#ifndef UPDATA_LOCKCHECK_H
#define UPDATA_LOCKCHECK_H

#include <windows.h>

#define LOCK_MAX_APPS 8

typedef struct {
    wchar_t path[MAX_PATH * 2];      /* 目标文件完整路径 */
    wchar_t rel[MAX_PATH];           /* 相对路径（显示用） */
    int     locked;
    int     appCount;                /* 占用进程数（可能为 0：占用但未识别出进程） */
    wchar_t apps[LOCK_MAX_APPS][MAX_PATH];
    DWORD   pids[LOCK_MAX_APPS];
} LockInfo;

/* 判断单个文件是否被占用（是否能被替换） */
int lock_is_file_locked(const wchar_t *path);

/* 批量检测，返回被占用文件数量。*out_list 由调用者 free */
int lock_check_files(const wchar_t **paths, const wchar_t **rels, int count, LockInfo **out_list);

/* 尝试结束占用进程，返回成功结束的进程数量 */
int lock_kill_infos(LockInfo *list, int count);

/* 打开任务管理器 */
void lock_open_task_manager(void);

#endif
