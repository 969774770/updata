/* SHA-256 实现（FIPS 180-4），无外部依赖 */
#include "sha256.h"
#include <windows.h>
#include <string.h>

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(Sha256 *ctx, const uint8_t *d)
{
    uint32_t a, b, c, e, f, g, h, i, j, t1, t2, m[64], ss;

    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((uint32_t)d[j] << 24) | ((uint32_t)d[j + 1] << 16) | ((uint32_t)d[j + 2] << 8) | ((uint32_t)d[j + 3]);
    for (; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; ss = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + k[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = ss + t1;
        ss = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += ss;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(Sha256 *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

void sha256_update(Sha256 *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;

    for (i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = p[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

void sha256_final(Sha256 *ctx, uint8_t out[32])
{
    uint32_t i = (uint32_t)ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        out[i]      = (uint8_t)(ctx->state[0] >> (24 - i * 8));
        out[i + 4]  = (uint8_t)(ctx->state[1] >> (24 - i * 8));
        out[i + 8]  = (uint8_t)(ctx->state[2] >> (24 - i * 8));
        out[i + 12] = (uint8_t)(ctx->state[3] >> (24 - i * 8));
        out[i + 16] = (uint8_t)(ctx->state[4] >> (24 - i * 8));
        out[i + 20] = (uint8_t)(ctx->state[5] >> (24 - i * 8));
        out[i + 24] = (uint8_t)(ctx->state[6] >> (24 - i * 8));
        out[i + 28] = (uint8_t)(ctx->state[7] >> (24 - i * 8));
    }
}

static void to_hex(const uint8_t *bin, size_t len, char *out)
{
    static const char *hexchars = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; ++i) {
        out[i * 2]     = hexchars[(bin[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hexchars[bin[i] & 0x0F];
    }
    out[len * 2] = 0;
}

int sha256_file_hex(const wchar_t *path, char out_hex[65], unsigned long long *out_size)
{
    HANDLE h;
    Sha256 ctx;
    uint8_t buf[64 * 1024];
    DWORD got;
    uint8_t digest[32];
    unsigned long long total = 0;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    sha256_init(&ctx);
    for (;;) {
        if (!ReadFile(h, buf, (DWORD)sizeof(buf), &got, NULL)) {
            CloseHandle(h);
            return -1;
        }
        if (got == 0) break;
        sha256_update(&ctx, buf, got);
        total += got;
    }
    CloseHandle(h);

    sha256_final(&ctx, digest);
    to_hex(digest, 32, out_hex);
    if (out_size) *out_size = total;
    return 0;
}
