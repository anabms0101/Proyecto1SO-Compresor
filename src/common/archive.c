#include "archive.h"
#include "fileutils.h"
#include "huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Helpers de escritura/lectura binaria portable (little-endian) --- */

static void write_u32(FILE *f, uint32_t v) {
    unsigned char b[4] = { (unsigned char)(v), (unsigned char)(v >> 8),
                            (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static void write_u64(FILE *f, uint64_t v) {
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(v >> (8 * i));
    fwrite(b, 1, 8, f);
}

static int read_u32(FILE *f, uint32_t *v) {
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) return -1;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

static int read_u64(FILE *f, uint64_t *v) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) return -1;
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) r |= ((uint64_t)b[i]) << (8 * i);
    *v = r;
    return 0;
}

int archive_compress_directory(const char *dir_path, const char *out_path, int recursive) {
    FileList files;
    list_directory_files(dir_path, recursive, &files);

    FILE *out = fopen(out_path, "wb");
    if (!out) { file_list_free(&files); return -1; }

    fwrite(ARCHIVE_MAGIC, 1, 4, out);
    write_u32(out, (uint32_t)files.count);

    for (int i = 0; i < files.count; i++) {
        char full_path[4096];
        join_path(full_path, sizeof(full_path), dir_path, files.paths[i]);

        unsigned char *content = NULL;
        uint64_t content_len = 0;
        if (read_entire_file(full_path, &content, &content_len) != 0) {
            fclose(out);
            file_list_free(&files);
            return -1;
        }

        unsigned char md5[MD5_DIGEST_SIZE];
        MD5_CTX ctx;
        md5_init(&ctx);
        md5_update(&ctx, content, content_len);
        md5_final(&ctx, md5);

        HuffmanResult hr;
        if (huffman_compress(content, content_len, &hr) != 0) {
            free(content);
            fclose(out);
            file_list_free(&files);
            return -1;
        }

        uint32_t name_len = (uint32_t)strlen(files.paths[i]);
        write_u32(out, name_len);
        fwrite(files.paths[i], 1, name_len, out);
        write_u64(out, content_len);
        fwrite(md5, 1, MD5_DIGEST_SIZE, out);
        for (int k = 0; k < 256; k++) write_u64(out, hr.freq[k]);
        write_u64(out, hr.bit_count);
        write_u64(out, hr.data_len);
        if (hr.data_len > 0) fwrite(hr.data, 1, hr.data_len, out);

        huffman_result_free(&hr);
        free(content);
    }

    fclose(out);
    file_list_free(&files);
    return 0;
}

int archive_extract(const char *archive_path, const char *out_dir,
                     int *verified_count, int *total_count) {
    FILE *in = fopen(archive_path, "rb");
    if (!in) return -1;

    char magic[4];
    if (fread(magic, 1, 4, in) != 4 || memcmp(magic, ARCHIVE_MAGIC, 4) != 0) {
        fclose(in);
        return -1;
    }

    uint32_t num_files;
    if (read_u32(in, &num_files) != 0) { fclose(in); return -1; }

    make_dirs_recursive(out_dir);

    int verified = 0;
    for (uint32_t i = 0; i < num_files; i++) {
        uint32_t name_len;
        if (read_u32(in, &name_len) != 0) { fclose(in); return -1; }

        char name[4096];
        if (name_len >= sizeof(name)) { fclose(in); return -1; }
        if (fread(name, 1, name_len, in) != name_len) { fclose(in); return -1; }
        name[name_len] = '\0';

        uint64_t original_size;
        if (read_u64(in, &original_size) != 0) { fclose(in); return -1; }

        unsigned char stored_md5[MD5_DIGEST_SIZE];
        if (fread(stored_md5, 1, MD5_DIGEST_SIZE, in) != MD5_DIGEST_SIZE) { fclose(in); return -1; }

        uint64_t freq[256];
        for (int k = 0; k < 256; k++) {
            if (read_u64(in, &freq[k]) != 0) { fclose(in); return -1; }
        }

        uint64_t bit_count, compressed_bytes;
        if (read_u64(in, &bit_count) != 0) { fclose(in); return -1; }
        if (read_u64(in, &compressed_bytes) != 0) { fclose(in); return -1; }

        unsigned char *compressed = NULL;
        if (compressed_bytes > 0) {
            compressed = malloc(compressed_bytes);
            if (fread(compressed, 1, compressed_bytes, in) != compressed_bytes) {
                free(compressed);
                fclose(in);
                return -1;
            }
        }

        unsigned char *original = NULL;
        if (original_size > 0) {
            original = malloc(original_size);
            if (huffman_decompress(freq, compressed, bit_count, original, original_size) != 0) {
                free(compressed);
                free(original);
                fclose(in);
                return -1;
            }
        }
        free(compressed);

        char out_path[4096];
        join_path(out_path, sizeof(out_path), out_dir, name);

        /* Crear subdirectorios intermedios si el nombre incluye "/" */
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

        if (memcmp(actual_md5, stored_md5, MD5_DIGEST_SIZE) == 0) {
            verified++;
        }

        free(original);
    }

    fclose(in);
    if (verified_count) *verified_count = verified;
    if (total_count) *total_count = (int)num_files;
    return 0;
}
