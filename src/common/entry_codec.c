#define _POSIX_C_SOURCE 200809L
#include "entry_codec.h"
#include "archive.h"
#include "fileutils.h"
#include "huffman.h"
#include "binio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int entry_encode(const char *dir_path, const char *rel_path, EncodedEntry *out) {
    char full_path[4096];
    join_path(full_path, sizeof(full_path), dir_path, rel_path);

    unsigned char *content = NULL;
    uint64_t content_len = 0;
    if (read_entire_file(full_path, &content, &content_len) != 0) return -1;

    unsigned char md5[MD5_DIGEST_SIZE];
    MD5_CTX ctx;
    md5_init(&ctx);
    md5_update(&ctx, content, content_len);
    md5_final(&ctx, md5);

    HuffmanResult hr;
    if (huffman_compress(content, content_len, &hr) != 0) {
        free(content);
        return -1;
    }

    char *membuf = NULL;
    size_t memsize = 0;
    FILE *mem = open_memstream(&membuf, &memsize);
    if (!mem) {
        huffman_result_free(&hr);
        free(content);
        return -1;
    }

    uint32_t name_len = (uint32_t)strlen(rel_path);
    write_u32(mem, name_len);
    fwrite(rel_path, 1, name_len, mem);
    write_u64(mem, content_len);
    fwrite(md5, 1, MD5_DIGEST_SIZE, mem);
    for (int k = 0; k < 256; k++) write_u64(mem, hr.freq[k]);
    write_u64(mem, hr.bit_count);
    write_u64(mem, hr.data_len);
    if (hr.data_len > 0) fwrite(hr.data, 1, hr.data_len, mem);

    fclose(mem); /* actualiza membuf/memsize con el contenido final */

    out->buf = membuf;
    out->len = memsize;

    huffman_result_free(&hr);
    free(content);
    return 0;
}

void entry_encoded_free(EncodedEntry *e) {
    if (!e) return;
    free(e->buf);
    e->buf = NULL;
    e->len = 0;
}

int entry_extract_one(FILE *in, const char *out_dir, int *ok) {
    *ok = 0;

    uint32_t name_len;
    if (read_u32(in, &name_len) != 0) return -1;

    char name[4096];
    if (name_len >= sizeof(name)) return -1;
    if (fread(name, 1, name_len, in) != name_len) return -1;
    name[name_len] = '\0';

    uint64_t original_size;
    if (read_u64(in, &original_size) != 0) return -1;

    unsigned char stored_md5[MD5_DIGEST_SIZE];
    if (fread(stored_md5, 1, MD5_DIGEST_SIZE, in) != MD5_DIGEST_SIZE) return -1;

    uint64_t freq[256];
    for (int k = 0; k < 256; k++) {
        if (read_u64(in, &freq[k]) != 0) return -1;
    }

    uint64_t bit_count, compressed_bytes;
    if (read_u64(in, &bit_count) != 0) return -1;
    if (read_u64(in, &compressed_bytes) != 0) return -1;

    unsigned char *compressed = NULL;
    if (compressed_bytes > 0) {
        compressed = malloc(compressed_bytes);
        if (fread(compressed, 1, compressed_bytes, in) != compressed_bytes) {
            free(compressed);
            return -1;
        }
    }

    unsigned char *original = NULL;
    if (original_size > 0) {
        original = malloc(original_size);
        if (huffman_decompress(freq, compressed, bit_count, original, original_size) != 0) {
            free(compressed);
            free(original);
            return -1;
        }
    }
    free(compressed);

    char out_path[4096];
    join_path(out_path, sizeof(out_path), out_dir, name);

    char dir_only[4096];
    strncpy(dir_only, out_path, sizeof(dir_only) - 1);
    dir_only[sizeof(dir_only) - 1] = '\0';
    char *last_slash = strrchr(dir_only, '/');
    if (last_slash) {
        *last_slash = '\0';
        make_dirs_recursive(dir_only);
    }

    write_entire_file(out_path, original, original_size);

    unsigned char actual_md5[MD5_DIGEST_SIZE];
    MD5_CTX ctx;
    md5_init(&ctx);
    md5_update(&ctx, original, original_size);
    md5_final(&ctx, actual_md5);

    if (memcmp(actual_md5, stored_md5, MD5_DIGEST_SIZE) == 0) *ok = 1;

    free(original);
    return 0;
}
