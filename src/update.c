/* 更新核心逻辑：
   - 拉取更新清单（JSON）
   - 按 SHA-256 与本地文件比对得出差异
   - 更新前检测文件占用（可交互解锁）
   - 多线程并发下载 + 校验 + 替换
   - 更新完成后启动 auto_run 指定的程序并附加命令行参数 */
#include "app.h"
#include "manifest.h"
#include "http.h"
#include "sha256.h"
#include "util.h"
#include "lockcheck.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define MAX_WORKERS 8
#define MAX_RETRY   3

AppState g_app;

static HWND   g_hwnd = NULL;
static HANDLE g_lockEvent = NULL;
static int    g_lockDecision = -1;
static volatile LONG g_nextTask = 0;

void post_status(HWND hwnd, const wchar_t *fmt, ...)
{
    wchar_t *buf;
    va_list ap;

    if (!hwnd) return;
    buf = (wchar_t *)malloc(sizeof(wchar_t) * 1024);
    if (!buf) return;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);
    buf[1023] = 0;
    if (!PostMessageW(hwnd, WM_UPD_STATUS, 0, (LPARAM)buf)) free(buf);
}

static wchar_t *dup_w(const wchar_t *s)
{
    size_t n;
    wchar_t *p;
    if (!s) s = L"";
    n = wcslen(s);
    p = (wchar_t *)malloc(sizeof(wchar_t) * (n + 1));
    if (p) wcscpy(p, s);
    return p;
}

/* 发送消息；接收方不存在（如 DLL 场景 hwnd 为 NULL）时释放附带的内存 */
static void post_msg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!PostMessageW(hwnd, msg, wp, lp)) free((void *)lp);
}

/* 把清单模块的进度文本转发到界面 */
static void status_to_ui(const wchar_t *text)
{
    wchar_t *buf = dup_w(text);
    if (buf && !PostMessageW(g_hwnd, WM_UPD_STATUS, 0, (LPARAM)buf)) free(buf);
}

void update_init(void)
{
    manifest_init();
    manifest_set_status_fn(status_to_ui);
    g_lockEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    memset(&g_app, 0, sizeof(g_app));
    g_app.stopFlag = 0;
    lstrcpynW(g_app.exeDir, util_target_dir(), MAX_PATH);
}

void update_free_all(void)
{
    manifest_free_files();
}

static int on_download_progress(void *user, unsigned long long got, unsigned long long total)
{
    FileTask *t = (FileTask *)user;
    (void)total;
    EnterCriticalSection(&g_statsLock);
    t->got = got;
    LeaveCriticalSection(&g_statsLock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 拉取线程：清单解析与差异比对在 manifest.c（与 DLL 共用）              */
/* ------------------------------------------------------------------ */
static DWORD WINAPI fetch_thread(LPVOID param)
{
    wchar_t err[512];

    (void)param;
    err[0] = 0;

    if (manifest_fetch(err, 512) != 0) {
        util_log(L"[检测更新] 失败: %s", err);
        post_msg(g_hwnd, WM_UPD_FETCH_FAIL, 0, (LPARAM)dup_w(err));
        return 0;
    }

    InterlockedExchange(&g_app.manifestReady, 1);
    PostMessageW(g_hwnd, WM_UPD_FETCH_DONE, 0, 0);
    return 0;
}

void update_start_fetch(HWND hwnd)
{
    HANDLE h;
    g_hwnd = hwnd;
    update_free_all();
    g_app.updateCount = 0;
    InterlockedExchange(&g_app.manifestReady, 0);
    InterlockedExchange(&g_app.finished, 0);
    InterlockedExchange(&g_app.stopFlag, 0);
    g_app.doneCount = 0;
    g_app.failCount = 0;
    g_app.launchedCount = 0;
    g_app.bytesTotal = 0;
    g_app.bytesDone = 0;
    g_app.speed = 0;

    h = CreateThread(NULL, 0, fetch_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

/* ------------------------------------------------------------------ */
/* 下载                                                                */
/* ------------------------------------------------------------------ */
static DWORD WINAPI worker_thread(LPVOID param)
{
    (void)param;
    for (;;) {
        FileTask *t;
        int index, attempt, ok = 0;
        wchar_t tmp[MAX_PATH * 2];
        wchar_t lasterr[256];

        if (InterlockedCompareExchange(&g_app.stopFlag, 0, 0)) break;

        index = (int)InterlockedIncrement(&g_nextTask) - 1;
        if (index >= g_app.fileCount) break;
        t = &g_app.files[index];
        if (!t->needUpdate || t->skipTask) continue;

        util_create_parent_dirs(t->fullPath);
        _snwprintf(tmp, MAX_PATH * 2, L"%s.updtmp", t->fullPath);
        tmp[MAX_PATH * 2 - 1] = 0;

        manifest_task_state(t, FS_DOWNLOAD, NULL);
        EnterCriticalSection(&g_statsLock);
        t->got = 0;
        LeaveCriticalSection(&g_statsLock);

        lasterr[0] = 0;
        for (attempt = 0; attempt < MAX_RETRY && !ok; ++attempt) {
            wchar_t err[512];
            int rc;
            char localHash[65];

            if (InterlockedCompareExchange(&g_app.stopFlag, 0, 0)) break;
            if (attempt > 0) {
                Sleep(500);
                EnterCriticalSection(&g_statsLock);
                t->got = 0;
                LeaveCriticalSection(&g_statsLock);
                t->retry = attempt;
            }
            err[0] = 0;
            rc = http_download(t->urlW, tmp, on_download_progress, t,
                               &g_app.stopFlag, err, 512);
            if (rc == HTTP_ERR_ABORT) break;

            if (rc != HTTP_OK) {
                lstrcpynW(lasterr, err[0] ? err : L"下载失败", 256);
                util_log(L"[下载失败] %s: %s (第 %d 次)", t->relW, lasterr, attempt + 1);
                continue;
            }

            manifest_task_state(t, FS_VERIFY, NULL);
            if (sha256_file_hex(tmp, localHash, NULL) != 0) {
                lstrcpynW(lasterr, L"校验临时文件失败", 256);
                DeleteFileW(tmp);
                continue;
            }
            if (_stricmp(localHash, t->hash) != 0) {
                lstrcpynW(lasterr, L"文件校验不一致（下载不完整或服务端已更新）", 256);
                util_log(L"[校验失败] %s 期望=%S 实际=%S", t->relW, t->hash, localHash);
                DeleteFileW(tmp);
                continue;
            }

            if (MoveFileExW(tmp, t->fullPath, MOVEFILE_REPLACE_EXISTING)) {
                ok = 1;
                break;
            }
            lstrcpynW(lasterr, L"替换文件失败（文件可能被占用）", 256);
            util_log(L"[替换失败] %s 错误=%lu", t->fullPath, GetLastError());
            DeleteFileW(tmp);
        }

        if (ok) {
            EnterCriticalSection(&g_statsLock);
            t->got = t->size;
            LeaveCriticalSection(&g_statsLock);
            manifest_task_state(t, FS_DONE, NULL);
            util_log(L"[更新完成] %s", t->relW);
        } else if (InterlockedCompareExchange(&g_app.stopFlag, 0, 0)) {
            manifest_task_state(t, FS_CANCEL, L"已取消");
            DeleteFileW(tmp);
        } else {
            manifest_task_state(t, FS_FAIL, lasterr[0] ? lasterr : L"下载失败");
        }
    }
    return 0;
}

static DWORD WINAPI controller_thread(LPVOID param)
{
    int i, n = 0, workers, todo = 0;
    HANDLE threads[MAX_WORKERS];
    const wchar_t **paths = NULL;
    const wchar_t **rels = NULL;
    LockInfo *lockList = NULL;
    int lockedCount = 0;

    (void)param;

    /* 1) 更新前检测文件占用 */
    post_status(g_hwnd, L"正在检测文件占用...");
    for (i = 0; i < g_app.fileCount; ++i) {
        if (g_app.files[i].needUpdate) n++;
    }
    if (n > 0) {
        paths = (const wchar_t **)calloc((size_t)n, sizeof(wchar_t *));
        rels = (const wchar_t **)calloc((size_t)n, sizeof(wchar_t *));
    }
    if (paths && rels) {
        int k = 0;
        for (i = 0; i < g_app.fileCount; ++i) {
            if (g_app.files[i].needUpdate) {
                paths[k] = g_app.files[i].fullPath;
                rels[k] = g_app.files[i].relW;
                k++;
            }
        }
        lockedCount = lock_check_files(paths, rels, n, &lockList);
    }
    if (paths) free(paths);
    if (rels) free(rels);

    if (lockedCount > 0) {
        g_lockDecision = -1;
        PostMessageW(g_hwnd, WM_UPD_LOCK_FOUND, (WPARAM)lockedCount, (LPARAM)lockList);
        WaitForSingleObject(g_lockEvent, INFINITE);
        if (g_lockDecision == 2) {
            free(lockList);
            g_app.cancelled = 1;
            InterlockedExchange(&g_app.running, 0);
            InterlockedExchange(&g_app.finished, 1);
            g_app.endTick = GetTickCount();
            post_status(g_hwnd, L"更新已被取消");
            PostMessageW(g_hwnd, WM_UPD_UPDATE_DONE, 0, 0);
            return 0;
        }
        if (g_lockDecision == 1) {
            /* 跳过被占用文件 */
            int k;
            for (k = 0; k < lockedCount; ++k) {
                int j;
                if (!lockList[k].locked) continue;
                for (j = 0; j < g_app.fileCount; ++j) {
                    if (_wcsicmp(g_app.files[j].fullPath, lockList[k].path) == 0) {
                        g_app.files[j].skipTask = 1;
                        manifest_task_state(&g_app.files[j], FS_LOCKED, L"文件被占用，已跳过");
                    }
                }
            }
            util_log(L"[占用] 用户选择跳过被占用文件");
        } else {
            util_log(L"[占用] 用户选择自动结束进程并继续");
        }
        free(lockList);
    }

    /* 2) 并发下载 */
    InterlockedExchange(&g_nextTask, 0);

    post_status(g_hwnd, L"正在下载更新文件...");
    for (i = 0; i < g_app.fileCount; ++i) {
        if (g_app.files[i].needUpdate && !g_app.files[i].skipTask) todo++;
    }
    workers = todo;
    if (workers > 4) workers = 4;
    if (workers > MAX_WORKERS) workers = MAX_WORKERS;
    g_app.threadCount = workers;

    for (i = 0; i < workers; ++i) {
        threads[i] = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
        if (!threads[i]) break;
    }
    if (i == 0) {
        worker_thread(NULL);
    } else {
        WaitForMultipleObjects((DWORD)i, threads, TRUE, INFINITE);
        while (i-- > 0) CloseHandle(threads[i]);
    }

    /* 3) 统计结果 */
    g_app.doneCount = 0;
    g_app.failCount = 0;
    g_app.skipCount = 0;
    for (i = 0; i < g_app.fileCount; ++i) {
        LONG st = g_app.files[i].state;
        if (!g_app.files[i].needUpdate) continue;
        if (st == FS_DONE) g_app.doneCount++;
        else if (st == FS_FAIL) g_app.failCount++;
        else if (st == FS_LOCKED) g_app.skipCount++;
    }
    if (InterlockedCompareExchange(&g_app.stopFlag, 0, 0)) g_app.cancelled = 1;

    InterlockedExchange(&g_app.running, 0);
    InterlockedExchange(&g_app.finished, 1);
    g_app.endTick = GetTickCount();
    util_log(L"[结果] 成功 %d，失败 %d，跳过 %d，耗时 %lu ms", g_app.doneCount, g_app.failCount,
             g_app.skipCount, (unsigned long)(g_app.endTick - g_app.startTick));

    PostMessageW(g_hwnd, WM_UPD_UPDATE_DONE, (WPARAM)(g_app.failCount + g_app.skipCount), 0);
    return 0;
}

void update_start_download(HWND hwnd)
{
    HANDLE h;
    g_hwnd = hwnd;
    if (!g_app.manifestReady || g_app.updateCount <= 0) return;
    if (InterlockedCompareExchange(&g_app.running, 1, 0) != 0) return;

    InterlockedExchange(&g_app.stopFlag, 0);
    InterlockedExchange(&g_app.finished, 0);
    g_app.cancelled = 0;
    g_app.startTick = GetTickCount();
    {
        int i;
        for (i = 0; i < g_app.fileCount; ++i) {
            if (g_app.files[i].needUpdate && !g_app.files[i].skipTask) {
                InterlockedExchange(&g_app.files[i].state, FS_WAIT);
                g_app.files[i].got = 0;
                g_app.files[i].speed = 0;
                g_app.files[i].lastGot = 0;
                g_app.files[i].retry = 0;
            }
        }
    }

    h = CreateThread(NULL, 0, controller_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

void update_stop(void)
{
    g_app.cancelled = 1;
    InterlockedExchange(&g_app.stopFlag, 1);
}

void update_lock_decision(int decision)
{
    g_lockDecision = decision;
    if (g_lockEvent) SetEvent(g_lockEvent);
}

int update_launch_autorun(void)
{
    int i, launched = 0;
    wchar_t noRun[8];

    if (GetEnvironmentVariableW(L"UPDATA_NO_AUTORUN", noRun, 8) > 0) {
        util_log(L"[自动运行] 环境变量 UPDATA_NO_AUTORUN 已设置，跳过自动启动");
        return 0;
    }

    for (i = 0; i < g_app.fileCount; ++i) {
        FileTask *t = &g_app.files[i];
        wchar_t cmd[MAX_PATH * 3];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;

        if (!t->autoRun || !t->needUpdate) continue;
        if (t->state != FS_DONE) continue;

        if (g_app.extraArgs[0]) _snwprintf(cmd, MAX_PATH * 3, L"\"%s\" %s", t->fullPath, g_app.extraArgs);
        else _snwprintf(cmd, MAX_PATH * 3, L"\"%s\"", t->fullPath);
        cmd[MAX_PATH * 3 - 1] = 0;

        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        if (CreateProcessW(t->fullPath, cmd, NULL, NULL, FALSE, 0, NULL, g_app.exeDir, &si, &pi)) {
            launched++;
            util_log(L"[自动运行] %s", cmd);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            util_log(L"[自动运行] 失败 %s 错误=%lu", cmd, GetLastError());
        }
    }
    g_app.launchedCount = launched;
    return launched;
}
