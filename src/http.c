/* WinHTTP 下载实现：支持 HTTPS、自动重定向、进度回调、中断、状态码检查 */
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdarg.h>
#include "http.h"
#include "util.h"

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

#define UA L"updata/1.0 (Windows)"

static void set_err(wchar_t *err, int cap, const wchar_t *fmt, ...)
{
    va_list ap;
    if (!err || cap <= 0) return;
    va_start(ap, fmt);
    _vsnwprintf(err, (size_t)(cap - 1), fmt, ap);
    va_end(ap);
    err[cap - 1] = 0;
}

static void append_last_error(wchar_t *err, int cap, const wchar_t *prefix, DWORD code)
{
    wchar_t *msg = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPWSTR)&msg, 0, NULL);
    if (msg) {
        size_t l = wcslen(msg);
        while (l > 0 && (msg[l - 1] == L'\r' || msg[l - 1] == L'\n' || msg[l - 1] == L' ')) msg[--l] = 0;
        set_err(err, cap, L"%s (错误码 %lu: %s)", prefix, code, msg);
        LocalFree(msg);
    } else {
        set_err(err, cap, L"%s (错误码 %lu)", prefix, code);
    }
}

typedef struct {
    HINTERNET session;
    HINTERNET connect;
    HINTERNET request;
} HttpConn;

static void conn_close(HttpConn *c)
{
    if (c->request) WinHttpCloseHandle(c->request);
    if (c->connect) WinHttpCloseHandle(c->connect);
    if (c->session) WinHttpCloseHandle(c->session);
    c->request = c->connect = c->session = NULL;
}

static int conn_open(const wchar_t *url, HttpConn *c, unsigned long long *out_total,
                     wchar_t *err, int errcap)
{
    URL_COMPONENTS uc;
    wchar_t host[256], urlpath[4096], extra[2048], scheme[16];
    wchar_t fullpath[6144];
    DWORD flags = 0;
    DWORD policy;
    int rc = HTTP_OK;

    ZeroMemory(c, sizeof(*c));
    ZeroMemory(&uc, sizeof(uc));
    ZeroMemory(host, sizeof(host));
    ZeroMemory(urlpath, sizeof(urlpath));
    ZeroMemory(extra, sizeof(extra));
    ZeroMemory(scheme, sizeof(scheme));

    uc.dwStructSize = sizeof(uc);
    uc.lpszScheme = scheme;      uc.dwSchemeLength = 16;
    uc.lpszHostName = host;      uc.dwHostNameLength = 256;
    uc.lpszUrlPath = urlpath;    uc.dwUrlPathLength = 4096;
    uc.lpszExtraInfo = extra;    uc.dwExtraInfoLength = 2048;

    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        set_err(err, errcap, L"更新地址格式不正确（需形如 https://主机/路径?参数）");
        return HTTP_ERR_URL;
    }
    if (_wcsicmp(scheme, L"https") == 0) flags = WINHTTP_FLAG_SECURE;

    _snwprintf(fullpath, 6144, L"%s%s", urlpath, extra);
    fullpath[6143] = 0;

    c->session = WinHttpOpen(UA, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c->session) {
        c->session = WinHttpOpen(UA, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!c->session) {
        append_last_error(err, errcap, L"初始化网络会话失败", GetLastError());
        return HTTP_ERR_OPEN;
    }

    WinHttpSetTimeouts(c->session, 15000, 15000, 30000, 60000);

    c->connect = WinHttpConnect(c->session, host, uc.nPort, 0);
    if (!c->connect) {
        append_last_error(err, errcap, L"连接服务器失败", GetLastError());
        rc = HTTP_ERR_CONNECT;
        goto fail;
    }

    c->request = WinHttpOpenRequest(c->connect, L"GET", fullpath, NULL, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!c->request) {
        append_last_error(err, errcap, L"创建请求失败", GetLastError());
        rc = HTTP_ERR_REQUEST;
        goto fail;
    }

    policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(c->request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    if (!WinHttpSendRequest(c->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        append_last_error(err, errcap, L"发送请求失败", GetLastError());
        rc = HTTP_ERR_SEND;
        goto fail;
    }

    if (!WinHttpReceiveResponse(c->request, NULL)) {
        append_last_error(err, errcap, L"读取服务器响应失败", GetLastError());
        rc = HTTP_ERR_SEND;
        goto fail;
    }

    {
        DWORD status = 0, len = sizeof(status);
        if (!WinHttpQueryHeaders(c->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX)) {
            append_last_error(err, errcap, L"读取响应状态码失败", GetLastError());
            rc = HTTP_ERR_STATUS;
            goto fail;
        }
        if (status < 200 || status >= 300) {
            set_err(err, errcap, L"服务器返回 HTTP %lu", status);
            rc = HTTP_ERR_STATUS;
            goto fail;
        }
    }

    if (out_total) {
        DWORD total = 0, len = sizeof(total);
        *out_total = 0;
        if (WinHttpQueryHeaders(c->request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &total, &len, WINHTTP_NO_HEADER_INDEX)) {
            *out_total = total;
        }
    }
    return HTTP_OK;

fail:
    conn_close(c);
    return rc;
}

int http_get_text(const wchar_t *url, char **out_buf, wchar_t *err, int errcap)
{
    HttpConn c;
    int rc;
    char *buf = NULL;
    size_t used = 0, cap = 0;
    unsigned long long total = 0;
    char chunk[16 * 1024];
    DWORD got = 0;

    if (!url || !out_buf) return HTTP_ERR_URL;
    *out_buf = NULL;

    rc = conn_open(url, &c, &total, err, errcap);
    if (rc != HTTP_OK) return rc;

    cap = (total > 0 && total < 64 * 1024 * 1024) ? (size_t)total + 1 : 64 * 1024;
    buf = (char *)malloc(cap);
    if (!buf) { conn_close(&c); return HTTP_ERR_MEM; }

    for (;;) {
        if (!WinHttpReadData(c.request, chunk, (DWORD)sizeof(chunk), &got)) {
            append_last_error(err, errcap, L"读取数据失败", GetLastError());
            rc = HTTP_ERR_READ;
            goto fail;
        }
        if (got == 0) break;
        if (used + got + 1 > cap) {
            size_t ncap = cap * 2 + got + 1;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) { rc = HTTP_ERR_MEM; goto fail; }
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + used, chunk, got);
        used += got;
    }
    buf[used] = 0;
    conn_close(&c);
    *out_buf = buf;
    return HTTP_OK;

fail:
    conn_close(&c);
    free(buf);
    return rc;
}

int http_download(const wchar_t *url, const wchar_t *dest, HttpProgressFn cb, void *user,
                  volatile LONG *stop, wchar_t *err, int errcap)
{
    HttpConn c;
    int rc;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    unsigned long long total = 0, done = 0;
    char *chunk = NULL;
    DWORD got = 0;

    if (!url || !dest) return HTTP_ERR_URL;

    rc = conn_open(url, &c, &total, err, errcap);
    if (rc != HTTP_OK) return rc;

    hFile = CreateFileW(dest, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        append_last_error(err, errcap, L"创建临时文件失败", GetLastError());
        conn_close(&c);
        return HTTP_ERR_FILE;
    }

    chunk = (char *)malloc(128 * 1024);
    if (!chunk) {
        CloseHandle(hFile);
        conn_close(&c);
        return HTTP_ERR_MEM;
    }

    if (cb) cb(user, 0, total);

    for (;;) {
        DWORD written = 0;
        if (stop && *stop) {
            rc = HTTP_ERR_ABORT;
            set_err(err, errcap, L"用户已取消下载");
            goto fail;
        }
        if (!WinHttpReadData(c.request, chunk, 128 * 1024, &got)) {
            append_last_error(err, errcap, L"读取数据失败", GetLastError());
            rc = HTTP_ERR_READ;
            goto fail;
        }
        if (got == 0) break;
        if (!WriteFile(hFile, chunk, got, &written, NULL) || written != got) {
            append_last_error(err, errcap, L"写入文件失败", GetLastError());
            rc = HTTP_ERR_FILE;
            goto fail;
        }
        done += got;
        if (cb && cb(user, done, total)) {
            rc = HTTP_ERR_ABORT;
            set_err(err, errcap, L"用户已取消下载");
            goto fail;
        }
    }

    CloseHandle(hFile);
    hFile = INVALID_HANDLE_VALUE;
    free(chunk);
    conn_close(&c);

    if (total > 0 && done != total) {
        set_err(err, errcap, L"下载数据不完整（%llu/%llu 字节）", done, total);
        DeleteFileW(dest);
        return HTTP_ERR_READ;
    }
    return HTTP_OK;

fail:
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    if (chunk) free(chunk);
    conn_close(&c);
    DeleteFileW(dest);
    return rc;
}
