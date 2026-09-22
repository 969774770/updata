#ifndef UPDATA_APP_H
#define UPDATA_APP_H

#include <windows.h>
#include "lockcheck.h"

/* 自定义消息 */
#define WM_UPD_FETCH_DONE  (WM_APP + 1)
#define WM_UPD_FETCH_FAIL  (WM_APP + 2)   /* lParam = wchar_t*（GUI 释放） */
#define WM_UPD_STATUS      (WM_APP + 3)   /* lParam = wchar_t*（GUI 释放） */
#define WM_UPD_LOCK_FOUND  (WM_APP + 4)   /* lParam = LockInfo* */
#define WM_UPD_UPDATE_DONE (WM_APP + 5)   /* wParam = 失败数量 */

/* 文件状态 */
#define FS_WAIT        0
#define FS_SKIP_ACTIVE 1
#define FS_CURRENT     2
#define FS_SKIP_EXISTS 3
#define FS_DOWNLOAD    4
#define FS_VERIFY      5
#define FS_DONE        6
#define FS_FAIL        7
#define FS_CANCEL      8
#define FS_LOCKED      9

#define MAX_FILES 4096

typedef struct {
    char     nameU8[MAX_PATH];
    char     relU8[MAX_PATH];
    char     hash[80];
    char     urlU8[2048];

    wchar_t  relW[MAX_PATH];
    wchar_t  nameW[MAX_PATH];
    wchar_t  fullPath[MAX_PATH * 2];
    wchar_t  errW[256];
    wchar_t  urlW[2048];

    unsigned long long size;
    int      active;
    int      autoRun;
    int      forceOverwrite;
    int      needUpdate;
    int      skipTask;             /* 被占用且用户选择跳过 */

    volatile LONG state;
    unsigned long long got;        /* 已下载字节（受 g_statsLock 保护） */
    unsigned long long lastGot;    /* GUI 采样用 */
    unsigned long long speed;      /* 当前速度 */
    int      retry;                /* 实际重试次数 */
} FileTask;

typedef struct {
    wchar_t  baseUrl[2048];
    wchar_t  extraArgs[1024];       /* 命令行第二参数 */
    wchar_t  exeDir[MAX_PATH];

    wchar_t  name[256];
    wchar_t  version[64];
    wchar_t  desc[1024];
    wchar_t  changelog[4096];
    wchar_t  status[512];

    FileTask *files;
    int      fileCount;
    int      updateCount;           /* 需要更新的文件数 */
    int      doneCount;
    int      failCount;
    int      skipCount;
    int      threadCount;

    volatile LONG running;
    volatile LONG stopFlag;
    volatile LONG manifestReady;
    volatile LONG finished;
    int      cancelled;
    int      launchedCount;

    unsigned long long bytesTotal;
    unsigned long long bytesDone;    /* 由 GUI 统计 */
    unsigned long long speed;

    DWORD    startTick;
    DWORD    endTick;
} AppState;

extern AppState g_app;
extern CRITICAL_SECTION g_statsLock;

/* ---- update.c ---- */
void update_init(void);
void update_start_fetch(HWND hwnd);
void update_start_download(HWND hwnd);
void update_stop(void);
void update_free_all(void);
void update_lock_decision(int decision);   /* 0=继续 1=跳过占用文件 2=取消 */
int  update_launch_autorun(void);          /* 启动 auto_run 文件，返回启动数量 */
void post_status(HWND hwnd, const wchar_t *fmt, ...);

#endif
