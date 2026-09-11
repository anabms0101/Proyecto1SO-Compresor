#include "archive.h"
#include "fileutils.h"
#include "huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

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

/* Escanea el .hzip y guarda el offset de inicio de cada entrada,
 * sin descomprimir nada todavia (solo para poder repartir el trabajo). */
static int build_index(const char *archive_path, long **out_offsets, uint32_t *out_count) {
    FILE *in = fopen(archive_path, "rb");
    if (!in) return -1;

    char magic[4];
    if (fread(magic, 1, 4, in) != 4 || memcmp(magic, ARCHIVE_MAGIC, 4) != 0) {
        fclose(in);
        return -1;
    }

    uint32_t num_files;
    if (read_u32(in, &num_files) != 0) { fclose(in); return -1; }

    long *offsets = malloc(sizeof(long) * (num_files > 0 ? num_files : 1));

    for (uint32_t i = 0; i < num_files; i++) {
        offsets[i] = ftell(in);

        uint32_t name_len;
        if (read_u32(in, &name_len) != 0) { fclose(in); free(offsets); return -1; }
        fseek(in, name_len, SEEK_CUR);

        uint64_t original_size;
        if (read_u64(in, &original_size) != 0) { fclose(in); free(offsets); return -1; }

        fseek(in, MD5_DIGEST_SIZE, SEEK_CUR);
        fseek(in, 256 * 8, SEEK_CUR); /* tabla de frecuencias */

        uint64_t bit_count, compressed_bytes;
        if (read_u64(in, &bit_count) != 0) { fclose(in); free(offsets); return -1; }
        if (read_u64(in, &compressed_bytes) != 0) { fclose(in); free(offsets); return -1; }

        fseek(in, (long)compressed_bytes, SEEK_CUR);
    }

    fclose(in);
    *out_offsets = offsets;
    *out_count = num_files;
    return 0;
}

/* Descomprime una entrada ya posicionada (in apunta justo antes de name_len). */
static int extract_one(FILE *in, const char *out_dir, int *ok) {
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

/* El hijo procesa su rango y reporta cuantas firmas verifico via pipe (IPC) */
static void worker_run(const char *archive_path, const char *out_dir,
                        long *offsets, int start, int end, int write_fd) {
    FILE *in = fopen(archive_path, "rb");
    int verified = 0;

    if (in) {
        for (int i = start; i < end; i++) {
            fseek(in, offsets[i], SEEK_SET);
            int ok = 0;
            if (extract_one(in, out_dir, &ok) == 0 && ok) verified++;
        }
        fclose(in);
    }

    write(write_fd, &verified, sizeof(int));
    close(write_fd);
    _exit(0);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <archivo.hzip> <directorio_destino> [num_procesos]\n", argv[0]);
        return 1;
    }

    const char *archive_path = argv[1];
    const char *out_dir = argv[2];
    int num_workers = (argc >= 4) ? atoi(argv[3]) : 4;
    if (num_workers < 1) num_workers = 1;
    if (num_workers > 32) num_workers = 32;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    long *offsets = NULL;
    uint32_t total = 0;
    if (build_index(archive_path, &offsets, &total) != 0) {
        fprintf(stderr, "Error al leer '%s'\n", archive_path);
        return 1;
    }

    if (total == 0) {
        printf("Archivo vacio, nada que descomprimir.\n");
        free(offsets);
        return 0;
    }

    make_dirs_recursive(out_dir);

    if ((uint32_t)num_workers > total) num_workers = (int)total;

    int base = total / num_workers;
    int extra = total % num_workers;

    int pipes[32][2];
    pid_t pids[32];
    int start = 0;

    for (int w = 0; w < num_workers; w++) {
        int count = base + (w < extra ? 1 : 0);
        int end = start + count;

        if (pipe(pipes[w]) != 0) { perror("pipe"); free(offsets); return 1; }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            free(offsets);
            return 1;
        } else if (pid == 0) {
            close(pipes[w][0]);
            worker_run(archive_path, out_dir, offsets, start, end, pipes[w][1]);
            /* worker_run llama _exit, no regresa */
        } else {
            close(pipes[w][1]);
            pids[w] = pid;
        }

        start = end;
    }

    int total_verified = 0;
    for (int w = 0; w < num_workers; w++) {
        int count = 0;
        read(pipes[w][0], &count, sizeof(int));
        close(pipes[w][0]);
        total_verified += count;
    }

    for (int w = 0; w < num_workers; w++) {
        int status;
        waitpid(pids[w], &status, 0);
    }

    free(offsets);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("Descompresion paralela (fork + pipes) completa: %s -> %s\n", archive_path, out_dir);
    printf("Procesos utilizados: %d\n", num_workers);
    printf("Firmas verificadas: %d/%u (%.2f%% de salud)\n",
           total_verified, total, total > 0 ? (100.0 * total_verified / total) : 100.0);
    printf("Tiempo total: %.4f s\n", elapsed);

    return 0;
}