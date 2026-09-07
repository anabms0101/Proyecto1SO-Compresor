#ifndef MD5_H
#define MD5_H

#include <stdint.h>
#include <stddef.h>

#define MD5_DIGEST_SIZE 16

typedef struct {
    uint32_t state[4];      /* A, B, C, D */
    uint64_t bit_count;     /* total bits procesados */
    unsigned char buffer[64];
    size_t buffer_len;
} MD5_CTX;

void md5_init(MD5_CTX *ctx);
void md5_update(MD5_CTX *ctx, const unsigned char *data, size_t len);
void md5_final(MD5_CTX *ctx, unsigned char digest[MD5_DIGEST_SIZE]);

/* Utilidad: calcula el MD5 de un archivo completo en disco. */
int md5_file(const char *path, unsigned char digest[MD5_DIGEST_SIZE]);

/* Convierte un digest de 16 bytes a su representacion hexadecimal (33 chars con \0). */
void md5_to_hex(const unsigned char digest[MD5_DIGEST_SIZE], char hex_out[33]);

#endif /* MD5_H */
