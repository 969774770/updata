/* 轻量 JSON 解析器：仅解析，不做任何外部依赖。支持 \uXXXX 转 UTF-8、代理对。 */
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    const char *p;
    const char *end;
    int         ok;
} Parser;

static JsonValue *parse_value(Parser *ps);

static JsonValue *jv_new(JsonType t)
{
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (v) v->type = t;
    return v;
}

void json_free(JsonValue *v)
{
    int i;
    if (!v) return;
    for (i = 0; i < v->count; ++i) {
        if (v->keys && v->keys[i]) free(v->keys[i]);
        if (v->items && v->items[i]) json_free(v->items[i]);
    }
    if (v->keys) free(v->keys);
    if (v->items) free(v->items);
    if (v->str) free(v->str);
    free(v);
}

static void skip_ws(Parser *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ps->p++;
        else break;
    }
}

static int hex4(const char *s, unsigned int *out)
{
    unsigned int v = 0;
    int i;
    for (i = 0; i < 4; ++i) {
        char c = s[i];
        unsigned int d;
        if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A' + 10);
        else return -1;
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

/* 追加一个原始字节（JSON 文本本身已是 UTF-8） */
static void raw_put(char **buf, size_t *len, size_t *cap, unsigned char b)
{
    if (*len + 2 >= *cap) {
        size_t nc = *cap ? *cap * 2 : 64;
        char *nb = (char *)realloc(*buf, nc);
        if (!nb) return;
        *buf = nb;
        *cap = nc;
    }
    (*buf)[(*len)++] = (char)b;
}

/* 追加一个 Unicode 码点（转成 UTF-8） */
static void utf8_put(char **buf, size_t *len, size_t *cap, unsigned int cp)
{
    if (*len + 4 >= *cap) {
        size_t nc = *cap ? *cap * 2 : 64;
        char *nb = (char *)realloc(*buf, nc);
        if (!nb) return;
        *buf = nb;
        *cap = nc;
    }
    if (cp < 0x80) {
        (*buf)[(*len)++] = (char)cp;
    } else if (cp < 0x800) {
        (*buf)[(*len)++] = (char)(0xC0 | (cp >> 6));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        (*buf)[(*len)++] = (char)(0xE0 | (cp >> 12));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        (*buf)[(*len)++] = (char)(0xF0 | (cp >> 18));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    }
}

/* 解析字符串，返回 UTF-8 缓冲区（调用者 free） */
static char *parse_string_raw(Parser *ps)
{
    char  *buf = NULL;
    size_t len = 0, cap = 0;

    if (ps->p >= ps->end || *ps->p != '"') { ps->ok = 0; return NULL; }
    ps->p++;

    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p;
        if (c == '"') {
            ps->p++;
            if (!buf) { buf = (char *)malloc(1); if (!buf) { ps->ok = 0; return NULL; } }
            else if (len + 1 > cap) {
                char *nb = (char *)realloc(buf, len + 1);
                if (!nb) { ps->ok = 0; free(buf); return NULL; }
                buf = nb;
            }
            buf[len] = 0;
            return buf;
        }
        if (c == '\\') {
            ps->p++;
            if (ps->p >= ps->end) break;
            switch (*ps->p) {
            case '"':  utf8_put(&buf, &len, &cap, '"');  ps->p++; break;
            case '\\': utf8_put(&buf, &len, &cap, '\\'); ps->p++; break;
            case '/':  utf8_put(&buf, &len, &cap, '/');  ps->p++; break;
            case 'b':  utf8_put(&buf, &len, &cap, '\b'); ps->p++; break;
            case 'f':  utf8_put(&buf, &len, &cap, '\f'); ps->p++; break;
            case 'n':  utf8_put(&buf, &len, &cap, '\n'); ps->p++; break;
            case 'r':  utf8_put(&buf, &len, &cap, '\r'); ps->p++; break;
            case 't':  utf8_put(&buf, &len, &cap, '\t'); ps->p++; break;
            case 'u': {
                unsigned int cp = 0;
                if (ps->p + 5 > ps->end || hex4(ps->p + 1, &cp) != 0) { ps->ok = 0; free(buf); return NULL; }
                ps->p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && ps->p + 6 <= ps->end &&
                    ps->p[0] == '\\' && ps->p[1] == 'u') {
                    unsigned int lo = 0;
                    if (hex4(ps->p + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        ps->p += 6;
                    }
                }
                utf8_put(&buf, &len, &cap, cp);
                break;
            }
            default:
                ps->ok = 0; free(buf); return NULL;
            }
            continue;
        }
        if (c < 0x80) utf8_put(&buf, &len, &cap, c);
        else raw_put(&buf, &len, &cap, c);   /* 多字节 UTF-8 原样保留 */
        ps->p++;
    }
    ps->ok = 0;
    free(buf);
    return NULL;
}

static JsonValue *parse_object(Parser *ps)
{
    JsonValue *v = jv_new(JSON_OBJECT);
    if (!v) { ps->ok = 0; return NULL; }
    ps->p++; /* { */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return v; }
    for (;;) {
        char *key;
        JsonValue *val;
        JsonValue **ni;
        char **nk;

        skip_ws(ps);
        key = parse_string_raw(ps);
        if (!ps->ok) { free(key); json_free(v); return NULL; }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') { free(key); ps->ok = 0; json_free(v); return NULL; }
        ps->p++;
        skip_ws(ps);
        val = parse_value(ps);
        if (!ps->ok) { free(key); json_free(v); return NULL; }

        ni = (JsonValue **)realloc(v->items, sizeof(JsonValue *) * (size_t)(v->count + 1));
        nk = (char **)realloc(v->keys, sizeof(char *) * (size_t)(v->count + 1));
        if (!ni || !nk) { free(key); json_free(val); json_free(v); ps->ok = 0; return NULL; }
        v->items = ni;
        v->keys = nk;
        v->keys[v->count] = key;
        v->items[v->count] = val;
        v->count++;

        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == '}') { ps->p++; return v; }
        ps->ok = 0;
        json_free(v);
        return NULL;
    }
}

static JsonValue *parse_array(Parser *ps)
{
    JsonValue *v = jv_new(JSON_ARRAY);
    if (!v) { ps->ok = 0; return NULL; }
    ps->p++; /* [ */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; return v; }
    for (;;) {
        JsonValue *val;
        JsonValue **ni;

        skip_ws(ps);
        val = parse_value(ps);
        if (!ps->ok) { json_free(v); return NULL; }

        ni = (JsonValue **)realloc(v->items, sizeof(JsonValue *) * (size_t)(v->count + 1));
        if (!ni) { json_free(val); json_free(v); ps->ok = 0; return NULL; }
        v->items = ni;
        v->items[v->count++] = val;

        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == ']') { ps->p++; return v; }
        ps->ok = 0;
        json_free(v);
        return NULL;
    }
}

static JsonValue *parse_value(Parser *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) { ps->ok = 0; return NULL; }

    switch (*ps->p) {
    case '{': return parse_object(ps);
    case '[': return parse_array(ps);
    case '"': {
        JsonValue *v = jv_new(JSON_STRING);
        if (!v) { ps->ok = 0; return NULL; }
        v->str = parse_string_raw(ps);
        if (!ps->ok) { json_free(v); return NULL; }
        return v;
    }
    case 't':
        if (ps->end - ps->p >= 4 && strncmp(ps->p, "true", 4) == 0) {
            JsonValue *v = jv_new(JSON_BOOL);
            if (!v) { ps->ok = 0; return NULL; }
            v->boolean = 1;
            ps->p += 4;
            return v;
        }
        break;
    case 'f':
        if (ps->end - ps->p >= 5 && strncmp(ps->p, "false", 5) == 0) {
            JsonValue *v = jv_new(JSON_BOOL);
            if (!v) { ps->ok = 0; return NULL; }
            v->boolean = 0;
            ps->p += 5;
            return v;
        }
        break;
    case 'n':
        if (ps->end - ps->p >= 4 && strncmp(ps->p, "null", 4) == 0) {
            JsonValue *v = jv_new(JSON_NULL);
            ps->p += 4;
            return v;
        }
        break;
    default:
        break;
    }

    /* 数字 */
    {
        const char *start = ps->p;
        char tmp[64];
        size_t n;
        JsonValue *v;

        if (*ps->p == '-' || *ps->p == '+') ps->p++;
        while (ps->p < ps->end && ((*ps->p >= '0' && *ps->p <= '9') || *ps->p == '.' ||
                                   *ps->p == 'e' || *ps->p == 'E' || *ps->p == '-' || *ps->p == '+'))
            ps->p++;
        n = (size_t)(ps->p - start);
        if (n == 0 || n >= sizeof(tmp)) { ps->ok = 0; return NULL; }
        memcpy(tmp, start, n);
        tmp[n] = 0;
        v = jv_new(JSON_NUMBER);
        if (!v) { ps->ok = 0; return NULL; }
        v->number = atof(tmp);
        return v;
    }
}

JsonValue *json_parse(const char *text)
{
    Parser ps;
    JsonValue *v;
    if (!text) return NULL;
    /* 跳过 UTF-8 BOM */
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text += 3;
    ps.p = text;
    ps.end = text + strlen(text);
    ps.ok = 1;
    v = parse_value(&ps);
    if (!ps.ok) { json_free(v); return NULL; }
    return v;
}

JsonValue *json_get(JsonValue *obj, const char *key)
{
    int i;
    if (!obj || obj->type != JSON_OBJECT || !obj->keys) return NULL;
    for (i = 0; i < obj->count; ++i) {
        if (obj->keys[i] && strcmp(obj->keys[i], key) == 0) return obj->items[i];
    }
    return NULL;
}

JsonValue *json_at(JsonValue *arr, int index)
{
    if (!arr || arr->type != JSON_ARRAY) return NULL;
    if (index < 0 || index >= arr->count) return NULL;
    return arr->items[index];
}

const char *json_string(JsonValue *v, const char *def)
{
    if (v && v->type == JSON_STRING && v->str) return v->str;
    return def;
}

double json_number(JsonValue *v, double def)
{
    if (v && v->type == JSON_NUMBER) return v->number;
    return def;
}

int json_bool(JsonValue *v, int def)
{
    if (v && v->type == JSON_BOOL) return v->boolean;
    if (v && v->type == JSON_NUMBER) return v->number != 0;
    return def;
}
