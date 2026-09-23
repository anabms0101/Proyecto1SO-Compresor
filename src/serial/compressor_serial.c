#include "archive.h"
#include "fileutils.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <directorio_origen> <archivo_salida.hzip> [--recursivo]\n", argv[0]);
        return 1;
    }

    const char *dir_path = argv[1];
    const char *out_path = argv[2];
    int recursive = (argc >= 4 && strcmp(argv[3], "--recursivo") == 0);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (archive_compress_directory(dir_path, out_path, recursive) != 0) {
        fprintf(stderr, "Error al comprimir '%s'\n", dir_path);
        printf("RESULT ok=0\n");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    uint64_t original_size = directory_total_size(dir_path, recursive);
    uint64_t compressed_size = file_size_bytes(out_path);

    printf("Compresion serial completa: %s -> %s\n", dir_path, out_path);
    printf("Tiempo total: %.4f s\n", elapsed);
    printf("Tamano original: %llu bytes\n", (unsigned long long)original_size);
    printf("Tamano comprimido: %llu bytes\n", (unsigned long long)compressed_size);
    if (original_size > 0) {
        printf("Radio de compresion: %.2f%%\n",
               100.0 * (double)compressed_size / (double)original_size);
    }

    /* Linea final en formato facil de parsear (usada por la GUI) */
    printf("RESULT ok=1 elapsed=%.6f original_size=%llu compressed_size=%llu\n",
           elapsed, (unsigned long long)original_size, (unsigned long long)compressed_size);

    return 0;
}
