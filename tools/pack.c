/* 构建工具：把 updata.exe 压缩成内嵌用的 payload.bin
   用法: pack.exe <输入文件> <输出文件>
   输出格式: "UPK1" + 原始长度(4字节LE) + FNV1a校验(4字节LE) + LZ 数据流
   LZ 数据流为 LZ4 block 风格（token + 字面量 + 2字节偏移 + 匹配长度），
   dll.c 中的 unpack() 与之对应；本工具会先自解压校验一遍再写出。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define HASH_BITS   14
#define HASH_SIZE   (1u << HASH_BITS)
#define MIN_MATCH   4
#define MAX_OFFSET  65535

/* ---------------- 输出缓冲 ---------------- */
static unsigned char *g_out;
static size_t g_len, g_cap;

static void out_reserve(size_t extra)
{
    if (g_len + extra <= g_cap) return;
    size_t nc = g_cap ? g_cap * 2 : 65536;
    while (nc < g_len + extra) nc *= 2;
    unsigned char *nb = (unsigned char *)realloc(g_out, nc);
    if (!nb) { fprintf(stderr, "内存不足\n"); exit(2); }
    g_out = nb;
    g_cap = nc;
}

static void put_byte(unsigned char b)
{
    out_reserve(1);
    g_out[g_len++] = b;
}

static void put_data(const unsigned char *p, size_t n)
{
    if (!n) return;
    out_reserve(n);
    memcpy(g_out + g_len, p, n);
    g_len += n;
}

/* ---------------- 校验 ---------------- */
static uint32_t fnv1a(const unsigned char *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* ---------------- 压缩 ---------------- */
static uint32_t hash4(const unsigned char *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - HASH_BITS);
}

static void emit_length(size_t v, size_t base)
{
    size_t r = v - base;
    while (r >= 255) { put_byte(255); r -= 255; }
    put_byte((unsigned char)r);
}

static void emit_sequence(size_t lit_len, const unsigned char *lit, size_t offset, size_t mlen)
{
    size_t ml = mlen - MIN_MATCH;
    put_byte((unsigned char)(((lit_len >= 15 ? 15 : lit_len) << 4) |
                             ((ml >= 15 ? 15 : ml) & 0x0F)));
    if (lit_len >= 15) emit_length(lit_len, 15);
    put_data(lit, lit_len);
    put_byte((unsigned char)(offset & 0xFF));
    put_byte((unsigned char)((offset >> 8) & 0xFF));
    if (ml >= 15) emit_length(ml, 15);
}

static void compress(const unsigned char *src, size_t n)
{
    uint32_t *htab = (uint32_t *)malloc(sizeof(uint32_t) * HASH_SIZE);
    size_t i = 0, anchor = 0;

    if (!htab) { fprintf(stderr, "内存不足\n"); exit(2); }
    memset(htab, 0xFF, sizeof(uint32_t) * HASH_SIZE);

    while (i + MIN_MATCH <= n) {
        uint32_t h = hash4(src + i);
        uint32_t cand = htab[h];
        htab[h] = (uint32_t)i;

        if (cand != 0xFFFFFFFFu && (size_t)cand < i && (i - cand) <= MAX_OFFSET &&
            memcmp(src + cand, src + i, MIN_MATCH) == 0) {
            size_t mlen = MIN_MATCH;
            size_t k;

            while (i + mlen < n && src[cand + mlen] == src[i + mlen]) mlen++;

            emit_sequence(i - anchor, src + anchor, i - cand, mlen);

            for (k = i + 1; k < i + mlen && k + MIN_MATCH <= n; ++k) {
                htab[hash4(src + k)] = (uint32_t)k;
            }
            i += mlen;
            anchor = i;
        } else {
            i++;
        }
    }

    if (anchor < n) {   /* 结尾字面量 */
        size_t lit = n - anchor;
        put_byte((unsigned char)((lit >= 15 ? 15 : lit) << 4));
        if (lit >= 15) emit_length(lit, 15);
        put_data(src + anchor, lit);
    }

    free(htab);
}

/* ---------------- 自解压校验（与 dll.c 的 unpack 同一算法） ---------------- */
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

int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *src = NULL, *chk = NULL;
    long n = 0;
    size_t got;
    uint32_t crc, crc2;
    unsigned char head[12];

    if (argc < 3) {
        fprintf(stderr, "用法: pack.exe <输入文件> <输出文件>\n");
        return 1;
    }

    f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "打不开 %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); fprintf(stderr, "文件为空\n"); return 1; }
    src = (unsigned char *)malloc((size_t)n);
    if (!src || fread(src, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); fprintf(stderr, "读取失败\n"); return 1;
    }
    fclose(f);

    crc = fnv1a(src, (size_t)n);
    compress(src, (size_t)n);

    /* 自解压校验 */
    chk = (unsigned char *)malloc((size_t)n);
    if (!chk) { fprintf(stderr, "内存不足\n"); return 2; }
    got = unpack(g_out, g_len, chk, (size_t)n);
    crc2 = (got == (size_t)n) ? fnv1a(chk, got) : 0;
    if (got != (size_t)n || crc2 != crc) {
        fprintf(stderr, "自检失败（解压结果与原始文件不一致），已中止\n");
        return 3;
    }

    memcpy(head, "UPK1", 4);
    head[4] = (unsigned char)(n & 0xFF);
    head[5] = (unsigned char)((n >> 8) & 0xFF);
    head[6] = (unsigned char)((n >> 16) & 0xFF);
    head[7] = (unsigned char)((n >> 24) & 0xFF);
    head[8] = (unsigned char)(crc & 0xFF);
    head[9] = (unsigned char)((crc >> 8) & 0xFF);
    head[10] = (unsigned char)((crc >> 16) & 0xFF);
    head[11] = (unsigned char)((crc >> 24) & 0xFF);

    f = fopen(argv[2], "wb");
    if (!f) { fprintf(stderr, "写不了 %s\n", argv[2]); return 1; }
    fwrite(head, 1, sizeof(head), f);
    fwrite(g_out, 1, g_len, f);
    fclose(f);

    printf("压缩完成: %ld -> %ld 字节 (%.1f%%)\n", n, (long)(g_len + sizeof(head)),
           (double)(g_len + sizeof(head)) * 100.0 / (double)n);
    free(src);
    free(chk);
    free(g_out);
    return 0;
}
