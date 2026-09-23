/*
Implementacion del algoritmo MD5 (RFC 1321), escrita a partir de la
especificacion del proyecto. No se copio codigo de terceros: las 
constantes (tabla K y desplazamientos S) provienen directamente del 
estandar publicado en el RFC 1321.
*/

#include "md5.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Tabla de constantes K[i] = floor(2^32 * abs(sin(i + 1))), i = 0..63 */
static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

/* Desplazamientos por ronda */
static const uint32_t S[64] = {
    7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
    5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
    4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
    6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
};

static uint32_t rotl32(uint32_t x, uint32_t c) {
    return (x << c) | (x >> (32 - c));
}

/* Procesa un bloque de 64 bytes y actualiza el estado */
static void md5_process_block(MD5_CTX *ctx, const unsigned char block[64]) {
    uint32_t M[16];
    for (int i = 0; i < 16; i++) {
        M[i] = (uint32_t)block[i * 4] |
               ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) |
               ((uint32_t)block[i * 4 + 3] << 24);
    }

    uint32_t A = ctx->state[0];
    uint32_t B = ctx->state[1];
    uint32_t C = ctx->state[2];
    uint32_t D = ctx->state[3];

    for (uint32_t i = 0; i < 64; i++) {
        uint32_t F;
        uint32_t g;

        if (i < 16) {
            F = (B & C) | (~B & D);
            g = i;
        } else if (i < 32) {
            F = (D & B) | (~D & C);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            F = B ^ C ^ D;
            g = (3 * i + 5) % 16;
        } else {
            F = C ^ (B | ~D);
            g = (7 * i) % 16;
        }

        F = F + A + K[i] + M[g];
        A = D;
        D = C;
        C = B;
        B = B + rotl32(F, S[i]);
    }

    ctx->state[0] += A;
    ctx->state[1] += B;
    ctx->state[2] += C;
    ctx->state[3] += D;
}

void md5_init(MD5_CTX *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
    ctx->bit_count = 0;
    ctx->buffer_len = 0;
}

void md5_update(MD5_CTX *ctx, const unsigned char *data, size_t len) {
    ctx->bit_count += (uint64_t)len * 8;

    while (len > 0) {
        size_t space = 64 - ctx->buffer_len;
        size_t take = (len < space) ? len : space;

        memcpy(ctx->buffer + ctx->buffer_len, data, take);
        ctx->buffer_len += take;
        data += take;
        len -= take;

        if (ctx->buffer_len == 64) {
            md5_process_block(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

/* Agrega un solo byte al buffer y procesa el bloque si se llena, sin
tocar bit_count (usado solo para el padding en md5_final). */
static void md5_feed_byte(MD5_CTX *ctx, unsigned char byte) {
    ctx->buffer[ctx->buffer_len++] = byte;
    if (ctx->buffer_len == 64) {
        md5_process_block(ctx, ctx->buffer);
        ctx->buffer_len = 0;
    }
}

void md5_final(MD5_CTX *ctx, unsigned char digest[MD5_DIGEST_SIZE]) {
    uint64_t bit_count_le = ctx->bit_count;

    md5_feed_byte(ctx, 0x80);

    while (ctx->buffer_len != 56) {
        md5_feed_byte(ctx, 0x00);
    }

    unsigned char len_bytes[8];
    for (int i = 0; i < 8; i++) {
        len_bytes[i] = (unsigned char)((bit_count_le >> (8 * i)) & 0xFF);
    }
    for (int i = 0; i < 8; i++) {
        md5_feed_byte(ctx, len_bytes[i]);
    }

    for (int i = 0; i < 4; i++) {
        digest[i * 4 + 0] = (unsigned char)(ctx->state[i] & 0xFF);
        digest[i * 4 + 1] = (unsigned char)((ctx->state[i] >> 8) & 0xFF);
        digest[i * 4 + 2] = (unsigned char)((ctx->state[i] >> 16) & 0xFF);
        digest[i * 4 + 3] = (unsigned char)((ctx->state[i] >> 24) & 0xFF);
    }
}

void md5_to_hex(const unsigned char digest[MD5_DIGEST_SIZE], char hex_out[33]) {
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < MD5_DIGEST_SIZE; i++) {
        hex_out[i * 2]     = hex_chars[(digest[i] >> 4) & 0x0F];
        hex_out[i * 2 + 1] = hex_chars[digest[i] & 0x0F];
    }
    hex_out[32] = '\0';
}

int md5_file(const char *path, unsigned char digest[MD5_DIGEST_SIZE]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    MD5_CTX ctx;
    md5_init(&ctx);

    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        md5_update(&ctx, buf, n);
    }

    if (ferror(f)) {
        fclose(f);
        return -1;
    }

    fclose(f);
    md5_final(&ctx, digest);
    return 0;
}
