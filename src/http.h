#ifndef UPDATA_HTTP_H
#define UPDATA_HTTP_H

#include <windows.h>

/* 进度回调：返回 0 继续下载，返回非 0 中止。total 为 0 表示未知长度 */
typedef int (*HttpProgressFn)(void *user, unsigned long long got, unsigned long long total);

/* 错误码 */
#define HTTP_OK           0
#define HTTP_ERR_URL     -1
#define HTTP_ERR_OPEN    -2
#define HTTP_ERR_CONNECT -3
#define HTTP_ERR_REQUEST -4
#define HTTP_ERR_SEND    -5
#define HTTP_ERR_STATUS  -6
#define HTTP_ERR_READ    -7
#define HTTP_ERR_FILE    -8
#define HTTP_ERR_MEM     -9
#define HTTP_ERR_ABORT  -10

/* GET 一个 URL 到内存（以 0 结尾的文本），成功返回 HTTP_OK；buf 由调用者 free */
int http_get_text(const wchar_t *url, char **out_buf, wchar_t *err, int errcap);

/* 下载 URL 到本地文件；stop 非空且变为非 0 时中止下载。成功返回 HTTP_OK */
int http_download(const wchar_t *url, const wchar_t *dest, HttpProgressFn cb, void *user,
                  volatile LONG *stop, wchar_t *err, int errcap);

#endif
