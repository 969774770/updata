#ifndef UPDATA_UTIL_H
#define UPDATA_UTIL_H

#include <windows.h>

/* UTF-8 <-> UTF-16 转换，返回的内存用 util_free 释放 */
wchar_t *util_u8_to_w(const char *s);
char    *util_w_to_u8(const wchar_t *s);
void     util_free(void *p);

/* 取 exe 所在目录（含结尾反斜杠），返回静态缓冲区 */
const wchar_t *util_exe_dir(void);

/* 取更新目标目录：优先环境变量 UPDATA_TARGET_DIR（DLL 把 exe 释放到临时目录时用），
   否则等于 exe 所在目录。返回静态缓冲区 */
const wchar_t *util_target_dir(void);

/* 拼接目录与相对路径（rel 中可用 / 或 \） */
wchar_t *util_path_join(const wchar_t *dir, const wchar_t *rel);

/* 为文件路径创建所需目录 */
int util_create_parent_dirs(const wchar_t *file_path);

int util_file_exists(const wchar_t *path);

/* 判断相对路径是否安全（不允许 ..、盘符、绝对路径） */
int util_is_safe_rel_path(const char *rel_utf8);

/* 以 base 为基准解析相对 URL（rel 可为绝对 URL、以 / 开头或相对路径） */
wchar_t *util_resolve_url(const wchar_t *base, const wchar_t *rel);

/* 格式化：字节数 / 速度 / 剩余时间 */
void util_fmt_size(unsigned long long bytes, wchar_t *out, int cap);
void util_fmt_speed(unsigned long long bps, wchar_t *out, int cap);
void util_fmt_time(unsigned long long secs, wchar_t *out, int cap);

/* 写日志到 exe 目录下 updata.log（UTF-8） */
void util_log(const wchar_t *fmt, ...);

#endif
