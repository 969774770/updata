/* updata32.dll / updata64.dll —— 更新检测模块（供易语言等宿主程序调用）
 *
 * 导出函数（__stdcall）：
 *   int updata(const char *url, const char *args);         // 参数为 ANSI/GBK 文本（易语言 文本型）
 *   int updata_w(const wchar_t *url, const wchar_t *args); // 参数为 Unicode 文本
 *
 * 返回值：
 *    0  已是最新：不做任何操作，宿主程序继续运行
 *    1  需要更新：已启动更新程序，并已结束当前进程（宿主程序不会继续执行）
 *   -1  检测失败（网络/服务器/超时）：不做任何操作
 *   -2  参数为空
 *   -3  更新程序释放或启动失败：不做任何操作
 *
 * 说明：
 *   - updata.exe 已作为资源内嵌在本 DLL 内，运行时释放到 %TEMP%\updata_run\ 再启动，
 *     因此本 DLL 是唯一需要分发的文件
 *   - 被更新的目录 = 宿主 exe 所在目录（通过环境变量 UPDATA_TARGET_DIR 告知更新程序）
 *   - 检测规则与 updata.exe 完全一致（共用 manifest.c），不会出现两边判断不一致
 *   - 检测过程不弹任何窗口；过程写宿主目录下的 updata.log（不含更新地址）
 */
#include <windows.h>
#include "app.h"
#include "manifest.h"
#include "util.h"
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

#define DLL_OK          0
#define DLL_UPDATED     1
#define DLL_ERR_CHECK  (-1)
#define DLL_ERR_PARAM  (-2)
#define DLL_ERR_EXE    (-3)

#define RES_UPDATA_EXE  101      /* 内嵌的 updata.exe 资源 ID（见 dll.rc） */
#define PAYLOAD_MAGIC   "UPK1"   /* 压缩载荷魔数：UPK1 + 原始长度 + 校验 + LZ 流 */
#define MIN_MATCH       4        /* 与 tools/pack.c 保持一致 */

AppState g_app;                  /* 清单模块使用（exe 里的定义在 update.c） */

static HINSTANCE     g_hinst = NULL;    /* DLL 自身模块句柄（查找自己的资源必须用它） */
static volatile LONG g_inited = 0;
static volatile LONG g_running = 0;

/* ---------------- 内嵌载荷解压（对应 tools/pack.c 的 LZ4 block 风格格式） ---------------- */
static unsigned int fnv1a(const unsigned char *p, size_t n)
{
    unsigned int h = 2166136261u;
    size_t i;
    for (i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static size_t unpack(const unsigned char *in, size_t in_len, unsigned char *out, size_t out_cap)
{
    const unsigned char *ip = in, *iend = in + in_len;
    unsigned char *op = out, *oend = out + out_cap;

    while (ip < iend) {
        unsigned int token = *ip++;
        size_t len = token >> 4;
        size_t offset, mlen, k;
        const unsigned char *mp;

        if (len == 15) {
            unsigned int b;
            do { if (ip >= iend) return 0; b = *ip++; len += b; } while (b == 255);
        }
        if (ip + len > iend || op + len > oend) return 0;
        memcpy(op, ip, len);
        op += len;
        ip += len;

        if (ip >= iend) break;

        if (ip + 2 > iend) return 0;
        offset = (size_t)ip[0] | ((size_t)ip[1] << 8);
        ip += 2;
        if (offset == 0 || offset > (size_t)(op - out)) return 0;

        mlen = (token & 0x0F) + MIN_MATCH;
        if ((token & 0x0F) == 15) {
            unsigned int b;
            do { if (ip >= iend) return 0; b = *ip++; mlen += b; } while (b == 255);
        }
        if (op + mlen > oend) return 0;

        mp = op - offset;
        for (k = 0; k < mlen; ++k) *op++ = *mp++;
    }
    return (size_t)(op - out);
}

/* DLL 侧初始化：只依赖清单模块，不含下载/解锁等 exe 专有逻辑 */
static void dll_init(void)
{
    manifest_init();
    memset(&g_app, 0, sizeof(g_app));
    lstrcpynW(g_app.exeDir, util_exe_dir(), MAX_PATH);   /* 更新目标 = 宿主 exe 目录 */
}

/* 把内存数据写成文件 */
static int write_all(const wchar_t *path, const void *data, DWORD size)
{
    HANDLE h;
    const BYTE *p = (const BYTE *)data;
    DWORD total = 0;

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    while (total < size) {
        DWORD written = 0;
        if (!WriteFile(h, p + total, size - total, &written, NULL) || written == 0) {
            CloseHandle(h);
            DeleteFileW(path);
            return 0;
        }
        total += written;
    }
    CloseHandle(h);
    return 1;
}

/* 释放内嵌的 updata.exe 到临时目录，成功返回 1 并写出完整路径 */
static int extract_updater(wchar_t *out_path, int cap)
{
    HRSRC res;
    HGLOBAL hg;
    const unsigned char *data;
    DWORD size;
    const unsigned char *payload;
    size_t payload_size;
    unsigned char *plain = NULL;
    wchar_t temp[MAX_PATH], dir[MAX_PATH];
    int pass;

    res = FindResourceW(g_hinst, MAKEINTRESOURCEW(RES_UPDATA_EXE), MAKEINTRESOURCEW(10));  /* 10 = RT_RCDATA */
    if (!res) {
        util_log(L"[DLL] 找不到内嵌的更新程序资源（错误 %lu）", GetLastError());
        return 0;
    }
    size = SizeofResource(g_hinst, res);
    hg = LoadResource(g_hinst, res);
    if (!hg || size == 0) {
        util_log(L"[DLL] 读取内嵌资源失败（错误 %lu）", GetLastError());
        return 0;
    }
    data = (const unsigned char *)LockResource(hg);
    if (!data) {
        util_log(L"[DLL] 锁定内嵌资源失败");
        return 0;
    }

    payload = data;
    payload_size = size;

    /* 压缩载荷：UPK1 + 原始长度(4) + 校验(4) + LZ 流；不是该格式则按原始内容处理 */
    if (size > 12 && memcmp(data, PAYLOAD_MAGIC, 4) == 0) {
        size_t orig = (size_t)data[4] | ((size_t)data[5] << 8) |
                      ((size_t)data[6] << 16) | ((size_t)data[7] << 24);
        unsigned int crc = (unsigned int)data[8] | ((unsigned int)data[9] << 8) |
                           ((unsigned int)data[10] << 16) | ((unsigned int)data[11] << 24);
        size_t got;

        plain = (unsigned char *)malloc(orig ? orig : 1);
        if (!plain) {
            util_log(L"[DLL] 内存不足，无法解压更新程序");
            return 0;
        }
        got = unpack(data + 12, (size_t)size - 12, plain, orig);
        if (got != orig || fnv1a(plain, got) != crc) {
            util_log(L"[DLL] 内嵌更新程序解压校验失败");
            free(plain);
            return 0;
        }
        payload = plain;
        payload_size = orig;
    }

    if (!GetTempPathW(MAX_PATH, temp)) {
        util_log(L"[DLL] 获取临时目录失败（错误 %lu）", GetLastError());
        if (plain) free(plain);
        return 0;
    }
    _snwprintf(dir, MAX_PATH, L"%supdata_run", temp);
    dir[MAX_PATH - 1] = 0;
    CreateDirectoryW(dir, NULL);

    for (pass = 0; pass < 2; ++pass) {
        wchar_t path[MAX_PATH];
        if (pass == 0) {
            _snwprintf(path, MAX_PATH, L"%s\\updata.exe", dir);      /* 常规：固定名，便于复用 */
        } else {
            _snwprintf(path, MAX_PATH, L"%s\\updata_%lu_%lu.exe", dir,      /* 被占用时换名 */
                       GetCurrentProcessId(), GetTickCount());
        }
        path[MAX_PATH - 1] = 0;
        if (write_all(path, payload, (DWORD)payload_size)) {
            lstrcpynW(out_path, path, cap);
            if (plain) free(plain);
            return 1;
        }
        util_log(L"[DLL] 释放到 %s 失败（错误 %lu）", path, GetLastError());
    }

    if (plain) free(plain);
    return 0;
}

/* 释放并启动更新程序：updata.exe "<更新地址>" <附加命令>，工作目录 = 宿主 exe 目录 */
static int launch_updater(const wchar_t *url_w, const wchar_t *args_w)
{
    wchar_t exe_path[MAX_PATH];
    wchar_t cmd[MAX_PATH * 4];
    wchar_t host_dir[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    exe_path[0] = 0;
    if (!extract_updater(exe_path, MAX_PATH)) {
        util_log(L"[DLL] 释放更新程序失败");
        return 0;
    }

    /* 更新目标目录 = 宿主 exe 目录，通过环境变量传给更新程序（子进程继承） */
    lstrcpynW(host_dir, util_exe_dir(), MAX_PATH);
    SetEnvironmentVariableW(L"UPDATA_TARGET_DIR", host_dir);

    _snwprintf(cmd, MAX_PATH * 4, L"\"%s\" \"%s\"", exe_path, url_w);
    cmd[MAX_PATH * 4 - 1] = 0;
    if (args_w && args_w[0]) {
        size_t len = wcslen(cmd);
        _snwprintf(cmd + len, MAX_PATH * 4 - len, L" %s", args_w);
        cmd[MAX_PATH * 4 - 1] = 0;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(exe_path, cmd, NULL, NULL, FALSE, 0, NULL, host_dir, &si, &pi)) {
        util_log(L"[DLL] 启动更新程序失败 错误=%lu", GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    util_log(L"[DLL] 已启动更新程序");
    return 1;
}

/* 同步检测：需要更新返回 1，不需要返回 0，失败返回负值 */
static int check_update(const wchar_t *url_w, const wchar_t *args_w)
{
    wchar_t err[512];
    int rc;

    if (!url_w || !url_w[0]) return DLL_ERR_PARAM;
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return DLL_ERR_CHECK;

    if (InterlockedCompareExchange(&g_inited, 1, 0) == 0) dll_init();

    lstrcpynW(g_app.baseUrl, url_w, 2048);
    lstrcpynW(g_app.extraArgs, (args_w && args_w[0]) ? args_w : L"", 1024);

    err[0] = 0;
    if (manifest_fetch(err, 512) != 0) {
        util_log(L"[DLL] 检测失败: %s", err);
        rc = DLL_ERR_CHECK;
        goto done;
    }

    if (g_app.updateCount <= 0) {
        util_log(L"[DLL] 已是最新版本，无需更新");
        rc = DLL_OK;
        goto done;
    }

    util_log(L"[DLL] 需要更新 %d 个文件", g_app.updateCount);
    rc = launch_updater(url_w, args_w) ? DLL_UPDATED : DLL_ERR_EXE;

done:
    InterlockedExchange(&g_running, 0);
    return rc;
}

/* ANSI/GBK -> Unicode（易语言 文本型传进来的是本机代码页的字节） */
static wchar_t *ansi_to_w(const char *s)
{
    int n;
    wchar_t *w;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
    if (!w) return NULL;
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    return w;
}

int __stdcall updata(const char *url, const char *args)
{
    wchar_t *url_w, *args_w;
    int rc;

    url_w = ansi_to_w(url);
    args_w = ansi_to_w(args);
    if (!url_w) {
        if (args_w) free(args_w);
        return DLL_ERR_PARAM;
    }

    rc = check_update(url_w, args_w);
    free(url_w);
    if (args_w) free(args_w);

    if (rc == DLL_UPDATED) {
        /* 立即结束自身进程，让更新程序可以覆盖/替换正在运行的文件 */
        util_log(L"[DLL] 结束当前进程以便完成更新");
        Sleep(50);
        ExitProcess(0);
    }
    return rc;
}

int __stdcall updata_w(const wchar_t *url, const wchar_t *args)
{
    int rc = check_update(url, args);
    if (rc == DLL_UPDATED) {
        util_log(L"[DLL] 结束当前进程以便完成更新");
        Sleep(50);
        ExitProcess(0);
    }
    return rc;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}
