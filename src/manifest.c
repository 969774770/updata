/* 更新清单的拉取与差异比对（updata.exe / updata.dll 共用）
   判断规则：
     - active=false            → 跳过（服务端已禁用）
     - 本地文件不存在           → 需要更新
     - 本地 hash 与服务端一致    → 已是最新
     - hash 不一致 且 强制覆盖   → 需要更新
     - hash 不一致 且 非强制覆盖 → 跳过（文件已存在即可） */
#include "manifest.h"
#include "json.h"
#include "http.h"
#include "sha256.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

CRITICAL_SECTION g_statsLock;

static ManifestStatusFn g_status_fn = NULL;

void manifest_init(void)
{
    InitializeCriticalSection(&g_statsLock);
}

void manifest_set_status_fn(ManifestStatusFn fn)
{
    g_status_fn = fn;
}

static void set_status(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;

    if (!g_status_fn) return;   /* DLL 无界面：不做任何显示 */
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);
    buf[1023] = 0;
    g_status_fn(buf);
}

void manifest_free_files(void)
{
    if (g_app.files) {
        free(g_app.files);
        g_app.files = NULL;
    }
    g_app.fileCount = 0;
}

void manifest_task_state(FileTask *t, int state, const wchar_t *err)
{
    EnterCriticalSection(&g_statsLock);
    if (err) lstrcpynW(t->errW, err, 256);
    else t->errW[0] = 0;
    InterlockedExchange(&t->state, state);
    LeaveCriticalSection(&g_statsLock);
}

static int json_str_to_w(const char *s, wchar_t *out, int cap)
{
    wchar_t *w;
    if (!s) s = "";
    w = util_u8_to_w(s);
    if (!w) { out[0] = 0; return -1; }
    lstrcpynW(out, w, cap);
    free(w);
    return 0;
}

static int fill_from_manifest(JsonValue *root, wchar_t *err, int errcap)
{
    JsonValue *data, *files, *f;
    const char *msg;
    int i, count;

    if (!json_bool(json_get(root, "ok"), 0)) {
        msg = json_string(json_get(root, "msg"), "");
        if (!msg[0]) msg = "服务端返回失败";
        json_str_to_w(msg, err, errcap);
        return -1;
    }

    data = json_get(root, "data");
    if (!data || data->type != JSON_OBJECT) {
        json_str_to_w("返回数据缺少 data 字段", err, errcap);
        return -1;
    }
    if (json_get(data, "error")) {
        json_str_to_w(json_string(json_get(data, "error"), "更新失败"), err, errcap);
        return -1;
    }

    json_str_to_w(json_string(json_get(data, "name"), "软件更新"), g_app.name, 256);
    json_str_to_w(json_string(json_get(data, "version"), ""), g_app.version, 64);
    json_str_to_w(json_string(json_get(data, "description"), ""), g_app.desc, 1024);
    json_str_to_w(json_string(json_get(data, "changelog"), ""), g_app.changelog, 4096);

    files = json_get(data, "files");
    count = (files && files->type == JSON_ARRAY) ? files->count : 0;
    if (count <= 0) {
        json_str_to_w("服务端未返回文件列表", err, errcap);
        return -1;
    }
    if (count > MAX_FILES) count = MAX_FILES;

    manifest_free_files();
    g_app.files = (FileTask *)calloc((size_t)count, sizeof(FileTask));
    if (!g_app.files) {
        json_str_to_w("内存不足", err, errcap);
        return -1;
    }

    for (i = 0; i < count; ++i) {
        FileTask *t = &g_app.files[g_app.fileCount];
        const char *cpath, *cname, *chash, *curl;
        char rel[512];
        wchar_t *wurl = NULL;
        char local_hash[65];
        char *u8;

        f = json_at(files, i);
        if (!f || f->type != JSON_OBJECT) continue;

        cpath = json_string(json_get(f, "path"), "");
        cname = json_string(json_get(f, "name"), "");
        chash = json_string(json_get(f, "hash"), "");
        curl  = json_string(json_get(f, "url"), "");
        if (!cname[0]) continue;

        if (cpath[0]) snprintf(rel, sizeof(rel), "%s/%s", cpath, cname);
        else snprintf(rel, sizeof(rel), "%s", cname);
        rel[sizeof(rel) - 1] = 0;

        if (!util_is_safe_rel_path(rel)) {
            wchar_t *wrel = util_u8_to_w(rel);
            util_log(L"[跳过] 非法文件路径: %s", wrel ? wrel : L"?");
            if (wrel) free(wrel);
            continue;
        }

        lstrcpynA(t->relU8, rel, MAX_PATH);
        lstrcpynA(t->nameU8, cname, MAX_PATH);
        lstrcpynA(t->hash, chash, 80);
        lstrcpynA(t->urlU8, curl, 2048);
        json_str_to_w(rel, t->relW, MAX_PATH);
        json_str_to_w(cname, t->nameW, MAX_PATH);

        t->size = (unsigned long long)json_number(json_get(f, "size"), 0);
        t->active = json_bool(json_get(f, "active"), 1);
        t->autoRun = json_bool(json_get(f, "auto_run"), 0);
        t->forceOverwrite = json_bool(json_get(f, "force_overwrite"), 0);

        {
            wchar_t *wrel = util_u8_to_w(rel);
            wchar_t *full = util_path_join(g_app.exeDir, wrel ? wrel : L"");
            if (full) {
                lstrcpynW(t->fullPath, full, MAX_PATH * 2);
                free(full);
            }
            if (wrel) free(wrel);
        }

        /* 解析下载地址（相对地址按接口地址转换） */
        wurl = util_u8_to_w(curl);
        if (wurl) {
            wchar_t *abs = util_resolve_url(g_app.baseUrl, wurl);
            if (abs) {
                u8 = util_w_to_u8(abs);
                if (u8) {
                    lstrcpynA(t->urlU8, u8, 2048);
                    free(u8);
                }
                lstrcpynW(t->urlW, abs, 2048);
                free(abs);
            }
            free(wurl);
        }

        g_app.fileCount++;

        /* 差异比对 */
        if (!t->active) {
            manifest_task_state(t, FS_SKIP_ACTIVE, NULL);
            continue;
        }
        if (!util_file_exists(t->fullPath)) {
            t->needUpdate = 1;
        } else if (sha256_file_hex(t->fullPath, local_hash, NULL) == 0 &&
                   _stricmp(local_hash, t->hash) == 0) {
            manifest_task_state(t, FS_CURRENT, NULL);
        } else if (t->forceOverwrite) {
            t->needUpdate = 1;
        } else {
            manifest_task_state(t, FS_SKIP_EXISTS, NULL);   /* 文件已存在且非强制覆盖 */
        }

        set_status(L"正在校验本地文件 %d/%d：%s", i + 1, count, t->nameW);
    }

    return 0;
}

void manifest_count(void)
{
    int i;
    unsigned long long total = 0;

    g_app.updateCount = 0;
    for (i = 0; i < g_app.fileCount; ++i) {
        if (g_app.files[i].needUpdate) {
            g_app.updateCount++;
            total += g_app.files[i].size;
        }
    }
    g_app.bytesTotal = total;

    util_log(L"[检测更新] 共 %d 个文件，需要更新 %d 个，共 %llu 字节",
             g_app.fileCount, g_app.updateCount, total);
}

int manifest_fetch(wchar_t *err, int errcap)
{
    char *text = NULL;
    JsonValue *root;

    if (err && errcap > 0) err[0] = 0;

    set_status(L"正在连接更新服务器...");
    util_log(L"[检测更新] 开始检测更新");   /* 不记录接口地址，避免泄露 software_key */

    if (http_get_text(g_app.baseUrl, &text, err, errcap) != HTTP_OK) {
        if (text) free(text);
        return -1;
    }

    set_status(L"正在解析更新信息...");
    root = json_parse(text);
    if (!root) {
        char head[121];
        int i;
        for (i = 0; i < 120 && text[i]; ++i) head[i] = (text[i] >= 32 && text[i] < 127) ? text[i] : '.';
        head[i] = 0;
        util_log(L"[检测更新] JSON 解析失败，响应内容: %S", head);
        free(text);
        if (err && errcap > 0) lstrcpynW(err, L"更新信息解析失败（返回内容不是合法 JSON）", errcap);
        return -1;
    }
    free(text);

    if (fill_from_manifest(root, err, errcap) != 0) {
        json_free(root);
        return -1;
    }
    json_free(root);

    manifest_count();
    return 0;
}
