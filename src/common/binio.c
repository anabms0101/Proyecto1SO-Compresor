#include "binio.h"

void write_u32(FILE *f, uint32_t v) {
    unsigned char b[4] = { (unsigned char)(v), (unsigned char)(v >> 8),
                            (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    fwrite(b, 1, 4, f);
}

void write_u64(FILE *f, uint64_t v) {
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(v >> (8 * i));
    fwrite(b, 1, 8, f);
}

int read_u32(FILE *f, uint32_t *v) {
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) return -1;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

int read_u64(FILE *f, uint64_t *v) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) return -1;
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) r |= ((uint64_t)b[i]) << (8 * i);
    *v = r;
    return 0;
}
