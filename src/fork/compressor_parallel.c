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

/* --- Helpers de escritura binaria (iguales a los de archive.c) --- */
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

/* Comprime un solo archivo y escribe su "entrada" (mismo formato que
   usa archive_compress_directory) en el FILE* out ya abierto. */
static int compress_one_entry(const char *dir_path, const char *rel_path, FILE *out) {
    char full_path[4096];
    join_path(full_path, sizeof(full_path), dir_path, rel_path);

    unsigned char *content = NULL;
    uint64_t content_len = 0;
    if (read_entire_file(full_path, &content, &content_len) != 0) {
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
        return -1;
    }

    uint32_t name_len = (uint32_t)strlen(rel_path);
    write_u32(out, name_len);
    fwrite(rel_path, 1, name_len, out);
    write_u64(out, content_len);
    fwrite(md5, 1, MD5_DIGEST_SIZE, out);
    for (int k = 0; k < 256; k++) write_u64(out, hr.freq[k]);
    write_u64(out, hr.bit_count);
    write_u64(out, hr.data_len);
    if (hr.data_len > 0) fwrite(hr.data, 1, hr.data_len, out);

    huffman_result_free(&hr);
    free(content);
    return 0;
}

/* Cada hijo comprime el rango [start, end) y escribe sus entradas
   en un archivo temporal propio. */
static int worker_run(const char *dir_path, FileList *files, int start, int end,
                       const char *tmp_path) {
    FILE *out = fopen(tmp_path, "wb");
    if (!out) return -1;

    for (int i = start; i < end; i++) {
        if (compress_one_entry(dir_path, files->paths[i], out) != 0) {
            fclose(out);
            return -1;
        }
    }

    fclose(out);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "Uso: %s <directorio_origen> <archivo_salida.hzip> [--recursivo] [num_procesos]\n",
            argv[0]);
        return 1;
    }

    const char *dir_path = argv[1];
    const char *out_path = argv[2];
    int recursive = (argc >= 4 && strcmp(argv[3], "--recursivo") == 0);
    int num_workers = (argc >= 5) ? atoi(argv[4]) : 4;
    if (num_workers < 1) num_workers = 1;
    if (num_workers > 64) num_workers = 64; /* limite de seguridad */

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    FileList files;
    list_directory_files(dir_path, recursive, &files);

    if (files.count == 0) {
        fprintf(stderr, "No hay archivos para comprimir en '%s'\n", dir_path);
        file_list_free(&files);
        return 1;
    }

    if (num_workers > files.count) num_workers = files.count;

    /* Repartir los archivos en rangos contiguos entre los procesos */
    int base = files.count / num_workers;
    int extra = files.count % num_workers;

    char tmp_paths[64][64];
    pid_t pids[64];
    int start = 0;

    for (int w = 0; w < num_workers; w++) {
        int count = base + (w < extra ? 1 : 0);
        int end = start + count;

        snprintf(tmp_paths[w], sizeof(tmp_paths[w]), "/tmp/hzip_part_%d_%d.tmp", getpid(), w);

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            file_list_free(&files);
            return 1;
        } else if (pid == 0) {
            /* --- Proceso hijo --- */
            int rc = worker_run(dir_path, &files, start, end, tmp_paths[w]);
            _exit(rc == 0 ? 0 : 1);
        } else {
            /* --- Proceso padre --- */
            pids[w] = pid;
        }

        start = end;
    }

    /* El padre espera a todos los hijos */
    int fail = 0;
    for (int w = 0; w < num_workers; w++) {
        int status;
        waitpid(pids[w], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) fail = 1;
    }

    if (fail) {
        fprintf(stderr, "Error: uno o mas procesos hijos fallaron al comprimir\n");
        for (int w = 0; w < num_workers; w++) remove(tmp_paths[w]);
        file_list_free(&files);
        return 1;
    }

    /* El padre une los archivos temporales en el .hzip final,
       respetando el orden original de los archivos */
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "No se pudo crear '%s'\n", out_path);
        for (int w = 0; w < num_workers; w++) remove(tmp_paths[w]);
        file_list_free(&files);
        return 1;
    }

    fwrite(ARCHIVE_MAGIC, 1, 4, out);
    write_u32(out, (uint32_t)files.count);

    unsigned char buf[65536];
    for (int w = 0; w < num_workers; w++) {
        FILE *part = fopen(tmp_paths[w], "rb");
        if (!part) {
            fclose(out);
            file_list_free(&files);
            return 1;
        }
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), part)) > 0) {
            fwrite(buf, 1, n, out);
        }
        fclose(part);
        remove(tmp_paths[w]);
    }

    fclose(out);
    file_list_free(&files);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("Compresion paralela (fork) completa: %s -> %s\n", dir_path, out_path);
    printf("Procesos utilizados: %d\n", num_workers);
    printf("Tiempo total: %.4f s\n", elapsed);

    return 0;
}
