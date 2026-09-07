#include "archive.h"
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
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("Compresion serial completa: %s -> %s\n", dir_path, out_path);
    printf("Tiempo total: %.4f s\n", elapsed);
    return 0;
}
