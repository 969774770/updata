#ifndef UPDATA_SHA256_H
#define UPDATA_SHA256_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    size_t   datalen;
} Sha256;

void sha256_init(Sha256 *ctx);
void sha256_update(Sha256 *ctx, const void *data, size_t len);
void sha256_final(Sha256 *ctx, uint8_t out[32]);

/* 计算文件 SHA-256，输出 64 字节小写十六进制字符串（含结尾 0，缓冲区至少 65 字节）。
   成功返回 0，失败返回 -1。out_size 可为 NULL。 */
int sha256_file_hex(const wchar_t *path, char out_hex[65], unsigned long long *out_size);

#endif
