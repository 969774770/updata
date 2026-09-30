#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

wchar_t *util_u8_to_w(const char *s)
{
    int n;
    wchar_t *w;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

char *util_w_to_u8(const wchar_t *s)
{
    int n;
    char *a;
    if (!s) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    a = (char *)malloc((size_t)n);
    if (!a) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, s, -1, a, n, NULL, NULL);
    return a;
}

void util_free(void *p)
{
    if (p) free(p);
}

const wchar_t *util_exe_dir(void)
{
    static wchar_t dir[MAX_PATH];
    static int inited = 0;
    if (!inited) {
        DWORD n;
        inited = 1;
        n = GetModuleFileNameW(NULL, dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) { dir[0] = L'.'; dir[1] = L'\\'; dir[2] = 0; }
        else {
            wchar_t *s = wcsrchr(dir, L'\\');
            if (s) *(s + 1) = 0;
            else wcscpy(dir, L".\\");
        }
    }
    return dir;
}

const wchar_t *util_target_dir(void)
{
    static wchar_t dir[MAX_PATH];
    static int inited = 0;

    if (!inited) {
        DWORD n = GetEnvironmentVariableW(L"UPDATA_TARGET_DIR", dir, MAX_PATH - 1);
        dir[MAX_PATH - 1] = 0;

        if (n > 0 && n < MAX_PATH - 1) {
            wchar_t *s = dir;
            wchar_t *q;
            size_t len;

            if (s[0] == L'"') s++;                  /* 允许带引号传入 */
            q = wcschr(s, L'"');
            if (q) *q = 0;

            len = wcslen(s);
            if (len == 0 || GetFileAttributesW(s) == INVALID_FILE_ATTRIBUTES) {
                lstrcpynW(dir, util_exe_dir(), MAX_PATH);   /* 目录无效则退回 exe 目录 */
            } else {
                if (s != dir) memmove(dir, s, sizeof(wchar_t) * (len + 1));
                if (dir[len - 1] != L'\\' && len + 2 < MAX_PATH) {
                    dir[len] = L'\\';
                    dir[len + 1] = 0;
                }
            }
        } else {
            lstrcpynW(dir, util_exe_dir(), MAX_PATH);
        }
        inited = 1;
    }
    return dir;
}

wchar_t *util_path_join(const wchar_t *dir, const wchar_t *rel)
{
    size_t dl, rl, i;
    wchar_t *out;

    if (!rel) return NULL;
    if (!dir) dir = L"";
    dl = wcslen(dir);
    rl = wcslen(rel);
    out = (wchar_t *)malloc(sizeof(wchar_t) * (dl + rl + 2));
    if (!out) return NULL;
    wcscpy(out, dir);
    for (i = 0; i < rl; ++i) out[dl + i] = (rel[i] == L'/' ? L'\\' : rel[i]);
    out[dl + rl] = 0;

    if (dl > 0 && out[dl - 1] != L'\\' && rl > 0 && rel[0] != L'\\') {
        memmove(out + dl + 1, out + dl, sizeof(wchar_t) * (rl + 1));
        out[dl] = L'\\';
    }
    return out;
}

int util_create_parent_dirs(const wchar_t *file_path)
{
    wchar_t buf[MAX_PATH * 2];
    wchar_t *p;

    if (!file_path) return -1;
    wcsncpy(buf, file_path, MAX_PATH * 2 - 1);
    buf[MAX_PATH * 2 - 1] = 0;

    p = wcsrchr(buf, L'\\');
    if (!p) return 0;
    *p = 0;
    if (buf[0] == 0) return 0;

    /* 逐级创建 */
    for (p = buf; *p; ++p) {
        if (*p == L'\\' && p != buf) {
            wchar_t saved = *p;
            *p = 0;
            if (wcslen(buf) > 2 || buf[1] != L':') CreateDirectoryW(buf, NULL);
            *p = saved;
        }
    }
    if (!CreateDirectoryW(buf, NULL)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) return -1;
    }
    return 0;
}

int util_file_exists(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

int util_is_safe_rel_path(const char *rel_utf8)
{
    const char *p;
    if (!rel_utf8 || !rel_utf8[0]) return 0;
    if (rel_utf8[0] == '/' || rel_utf8[0] == '\\') return 0;
    if (strstr(rel_utf8, "..")) return 0;
    if (rel_utf8[1] == ':') return 0;
    for (p = rel_utf8; *p; ++p) {
        if (*p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') return 0;
    }
    return 1;
}

wchar_t *util_resolve_url(const wchar_t *base, const wchar_t *rel)
{
    wchar_t *out;
    size_t need;

    if (!rel || !rel[0]) return NULL;

    /* 已是完整 URL */
    if (_wcsnicmp(rel, L"http://", 7) == 0 || _wcsnicmp(rel, L"https://", 8) == 0) {
        out = (wchar_t *)malloc(sizeof(wchar_t) * (wcslen(rel) + 1));
        if (out) wcscpy(out, rel);
        return out;
    }

    if (!base) return NULL;

    if (rel[0] == L'/') {
        /* 取 base 的 scheme://host[:port] */
        const wchar_t *p = wcsstr(base, L"://");
        const wchar_t *host_start;
        const wchar_t *slash;
        size_t prefix_len;
        if (!p) return NULL;
        host_start = p + 3;
        slash = wcschr(host_start, L'/');
        prefix_len = slash ? (size_t)(slash - base) : wcslen(base);
        need = prefix_len + wcslen(rel) + 2;
        out = (wchar_t *)malloc(sizeof(wchar_t) * need);
        if (!out) return NULL;
        wcsncpy(out, base, prefix_len);
        out[prefix_len] = 0;
        wcscat(out, rel);
        return out;
    }

    /* 相对路径：去掉 base 最后一段 */
    {
        const wchar_t *query = wcschr(base, L'?');
        const wchar_t *last_slash = NULL;
        const wchar_t *it;
        size_t prefix_len;

        for (it = base; query ? it < query : *it; ++it) {
            if (*it == L'/') last_slash = it;
        }
        if (!last_slash) return NULL;
        prefix_len = (size_t)(last_slash - base) + 1;
        need = prefix_len + wcslen(rel) + 2;
        out = (wchar_t *)malloc(sizeof(wchar_t) * need);
        if (!out) return NULL;
        wcsncpy(out, base, prefix_len);
        out[prefix_len] = 0;
        wcscat(out, rel);
        return out;
    }
}

static void fmt_num(double v, const wchar_t *unit, wchar_t *out, int cap, int decimals)
{
    swprintf(out, (size_t)cap, L"%.*f %s", decimals, v, unit);
}

void util_fmt_size(unsigned long long bytes, wchar_t *out, int cap)
{
    double v = (double)bytes;
    if (bytes < 1024ull) { fmt_num(v, L"B", out, cap, 0); return; }
    if (bytes < 1024ull * 1024) { fmt_num(v / 1024.0, L"KB", out, cap, 1); return; }
    if (bytes < 1024ull * 1024 * 1024) { fmt_num(v / (1024.0 * 1024.0), L"MB", out, cap, 2); return; }
    fmt_num(v / (1024.0 * 1024.0 * 1024.0), L"GB", out, cap, 2);
}

void util_fmt_speed(unsigned long long bps, wchar_t *out, int cap)
{
    wchar_t tmp[64];
    util_fmt_size(bps, tmp, 64);
    swprintf(out, (size_t)cap, L"%s/s", tmp);
}

void util_fmt_time(unsigned long long secs, wchar_t *out, int cap)
{
    if (secs < 60) swprintf(out, (size_t)cap, L"%llu 秒", secs);
    else if (secs < 3600) swprintf(out, (size_t)cap, L"%llu 分 %llu 秒", secs / 60, secs % 60);
    else swprintf(out, (size_t)cap, L"%llu 时 %llu 分", secs / 3600, (secs % 3600) / 60);
}

void util_log(const wchar_t *fmt, ...)
{
    static wchar_t path[MAX_PATH];
    static int path_ready = 0;
    wchar_t wline[4096];
    wchar_t stamp[64];
    SYSTEMTIME st;
    va_list ap;
    FILE *f;
    char *utf8;

    if (!path_ready) {
        /* 日志写到更新目标目录（DLL 把 exe 释放到临时目录时，日志仍留在软件目录） */
        _snwprintf(path, MAX_PATH, L"%s%s", util_target_dir(), L"updata.log");
        path[MAX_PATH - 1] = 0;
        path_ready = 1;
    }

    va_start(ap, fmt);
    _vsnwprintf(wline, 4000, fmt, ap);
    va_end(ap);
    wline[4000] = 0;

    GetLocalTime(&st);
    _snwprintf(stamp, 64, L"[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    stamp[63] = 0;

    {
        wchar_t all[4200];
        _snwprintf(all, 4200, L"%s%s\n", stamp, wline);
        all[4199] = 0;
        utf8 = util_w_to_u8(all);
    }
    if (!utf8) return;

    f = _wfopen(path, L"ab");
    if (!f) { free(utf8); return; }
    (void)fwrite(utf8, 1, strlen(utf8), f);
    fclose(f);
    free(utf8);
}

/* DLL 把 exe 释放到 %TEMP%\updata_run\ 后启动本程序。临时副本的收尾分两步：
     util_cleanup_self_copy()     启动时：清掉以往遗留下来的旧副本
     util_schedule_self_delete()  退出前：安排删除当前这个副本
   只有“运行在临时副本里”时才动手：exe 所在目录与更新目标目录不同即为该情况；
   直接放在软件目录里运行（两者相同）不做任何清理。 */

static int g_self_is_temp_copy = 0;

void util_cleanup_self_copy(void)
{
    const wchar_t *exe_dir = util_exe_dir();
    const wchar_t *tgt_dir = util_target_dir();
    wchar_t self[MAX_PATH];
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int removed = 0;

    if (_wcsicmp(exe_dir, tgt_dir) == 0) return;          /* 直接运行在软件目录：不动任何文件 */
    g_self_is_temp_copy = 1;
    if (GetModuleFileNameW(NULL, self, MAX_PATH) == 0) return;

    _snwprintf(pattern, MAX_PATH, L"%supdata*.exe", exe_dir);
    pattern[MAX_PATH - 1] = 0;

    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t full[MAX_PATH];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            _snwprintf(full, MAX_PATH, L"%s%s", exe_dir, fd.cFileName);
            full[MAX_PATH - 1] = 0;
            if (_wcsicmp(full, self) == 0) continue;      /* 自己由退出时的删除处理 */
            if (DeleteFileW(full)) removed++;             /* 正在被占用则删除失败，跳过即可 */
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (removed > 0) util_log(L"[清理] 已删除 %d 个遗留的更新程序副本", removed);
}

void util_schedule_self_delete(void)
{
    wchar_t self[MAX_PATH];
    wchar_t cmd[MAX_PATH * 2];
    wchar_t comspec[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (!g_self_is_temp_copy) return;
    if (GetModuleFileNameW(NULL, self, MAX_PATH) == 0) return;

    /* 运行中的 exe 无法删除自己（DeleteFile / DELETE_ON_CLOSE 都会被系统拒绝，
       父进程替它设置也一样），因此交给一个短命 cmd：等约 2 秒（本进程应已退出）后删除。
       万一没删掉，下次运行时的清扫会兜底。 */
    _snwprintf(cmd, MAX_PATH * 2, L"/c ping -n 3 127.0.0.1 >nul & del /f /q \"%s\"", self);
    cmd[MAX_PATH * 2 - 1] = 0;

    if (GetEnvironmentVariableW(L"COMSPEC", comspec, MAX_PATH) == 0) lstrcpynW(comspec, L"cmd.exe", MAX_PATH);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(comspec, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        util_log(L"[清理] 已安排退出后删除临时副本");
    } else {
        util_log(L"[清理] 安排删除临时副本失败（错误 %lu），下次运行时会清理", GetLastError());
    }
}
