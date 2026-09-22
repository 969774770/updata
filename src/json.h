#ifndef UPDATA_JSON_H
#define UPDATA_JSON_H

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

struct JsonValue {
    JsonType type;
    double   number;    /* JSON_NUMBER */
    int      boolean;   /* JSON_BOOL  */
    char    *str;       /* JSON_STRING 的 UTF-8 内容 / JSON_OBJECT 的键 */
    JsonValue **items;  /* 数组元素或对象值 */
    char    **keys;     /* 对象键（与 items 一一对应） */
    int      count;
};

/* 解析 JSON 文本，失败返回 NULL。返回值需用 json_free 释放。 */
JsonValue *json_parse(const char *text);

void json_free(JsonValue *v);

/* 对象取值，找不到返回 NULL */
JsonValue *json_get(JsonValue *obj, const char *key);
/* 数组取值 */
JsonValue *json_at(JsonValue *arr, int index);

const char *json_string(JsonValue *v, const char *def);
double json_number(JsonValue *v, double def);
int json_bool(JsonValue *v, int def);

#endif
