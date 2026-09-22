/* 文件占用检测与解锁：
   1) 用 CreateFileW(DELETE + 共享所有) 判断文件能否被替换；
   2) 用 Restart Manager 找出占用该文件的进程；
   3) Restart Manager 无结果时，用进程模块快照兜底（针对 exe/dll）；
   4) 支持直接结束占用进程。 */
#include "lockcheck.h"
#include "util.h"
#include <restartmanager.h>
#include <tlhelp32.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int lock_is_file_locked(const wchar_t *path)
{
    HANDLE h;
    DWORD e;

    /* 需要一个允许删除的句柄才能替换文件；失败即认为被占用 */
    h = CreateFileW(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return 0;
    }

    e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return 0;
    return 1;
}

static int add_app(LockInfo *info, const wchar_t *app, DWORD pid)
{
    int i;
    if (info->appCount >= LOCK_MAX_APPS) return 0;
    for (i = 0; i < info->appCount; ++i) {
        if (info->pids[i] == pid) return 0;
    }
    if (app && app[0]) {
        wcsncpy(info->apps[info->appCount], app, MAX_PATH - 1);
        info->apps[info->appCount][MAX_PATH - 1] = 0;
    } else {
        _snwprintf(info->apps[info->appCount], MAX_PATH, L"PID %lu", pid);
    }
    info->pids[info->appCount] = pid;
    info->appCount++;
    return 1;
}

/* Restart Manager 查询占用进程 */
static int query_restart_manager(const wchar_t *path, LockInfo *info)
{
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1];
    const wchar_t *files[1];
    UINT needed = 0, count = 0, i;
    RM_PROCESS_INFO infos[LOCK_MAX_APPS];
    DWORD reboot = 0, rc;
    int found = 0;

    ZeroMemory(key, sizeof(key));
    rc = RmStartSession(&session, 0, key);
    if (rc != ERROR_SUCCESS) return 0;

    files[0] = path;
    rc = RmRegisterResources(session, 1, files, 0, NULL, 0, NULL);
    if (rc != ERROR_SUCCESS) {
        RmEndSession(session);
        return 0;
    }

    count = LOCK_MAX_APPS;
    rc = RmGetList(session, &needed, &count, infos, &reboot);
    if (rc == ERROR_SUCCESS || rc == ERROR_MORE_DATA) {
        for (i = 0; i < count; ++i) {
            if (infos[i].Process.dwProcessId == GetCurrentProcessId()) continue;
            if (add_app(info, infos[i].strAppName, infos[i].Process.dwProcessId)) found = 1;
        }
    }
    RmEndSession(session);
    return found;
}

/* 兜底：遍历进程模块，找出加载了该文件的进程 */
static int query_modules(const wchar_t *path, LockInfo *info)
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    int found = 0;
    const wchar_t *base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            HANDLE mods;
            MODULEENTRY32W me;

            if (pe.th32ProcessID == GetCurrentProcessId()) continue;

            mods = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pe.th32ProcessID);
            if (mods == INVALID_HANDLE_VALUE) continue;

            ZeroMemory(&me, sizeof(me));
            me.dwSize = sizeof(me);
            if (Module32FirstW(mods, &me)) {
                do {
                    if (_wcsicmp(me.szModule, base) == 0) {
                        if (add_app(info, pe.szExeFile, pe.th32ProcessID)) found = 1;
                        break;
                    }
                } while (Module32NextW(mods, &me));
            }
            CloseHandle(mods);
            if (found && info->appCount >= LOCK_MAX_APPS) break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    (void)found;
    return info->appCount > 0;
}

int lock_check_files(const wchar_t **paths, const wchar_t **rels, int count, LockInfo **out_list)
{
    LockInfo *list;
    int i, lockedCount = 0;

    *out_list = NULL;
    if (count <= 0) return 0;

    list = (LockInfo *)calloc((size_t)count, sizeof(LockInfo));
    if (!list) return 0;

    for (i = 0; i < count; ++i) {
        LockInfo *info = &list[i];
        wcsncpy(info->path, paths[i], MAX_PATH * 2 - 1);
        if (rels && rels[i]) {
            wcsncpy(info->rel, rels[i], MAX_PATH - 1);
        }

        if (!lock_is_file_locked(paths[i])) continue;

        info->locked = 1;
        lockedCount++;

        if (!query_restart_manager(paths[i], info)) {
            query_modules(paths[i], info);
        }
        util_log(L"[占用] %s 被 %d 个进程占用", info->path, info->appCount);
    }

    *out_list = list;
    return lockedCount;
}

int lock_kill_infos(LockInfo *list, int count)
{
    int i, k, killed = 0;

    for (i = 0; i < count; ++i) {
        LockInfo *info = &list[i];
        if (!info->locked) continue;
        for (k = 0; k < info->appCount; ++k) {
            DWORD pid = info->pids[k];
            HANDLE h;
            if (pid == 0 || pid == GetCurrentProcessId()) continue;
            if (pid <= 4) continue;   /* 系统关键进程不处理 */

            h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (!h) {
                util_log(L"[结束进程] 打开进程失败 PID=%lu 错误=%lu", pid, GetLastError());
                continue;
            }
            if (TerminateProcess(h, 1)) {
                killed++;
                util_log(L"[结束进程] 已结束 %s (PID=%lu)", info->apps[k], pid);
            } else {
                util_log(L"[结束进程] 结束失败 %s (PID=%lu) 错误=%lu", info->apps[k], pid, GetLastError());
            }
            CloseHandle(h);
        }
    }

    if (killed > 0) Sleep(500);
    return killed;
}

void lock_open_task_manager(void)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t cmd[MAX_PATH];

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    _snwprintf(cmd, MAX_PATH, L"taskmgr.exe");
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}
