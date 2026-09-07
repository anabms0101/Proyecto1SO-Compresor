#include "archive.h"
#include <stdio.h>
#include <time.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <archivo.hzip> <directorio_destino>\n", argv[0]);
        return 1;
    }

    const char *archive_path = argv[1];
    const char *out_dir = argv[2];

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int verified = 0, total = 0;
    if (archive_extract(archive_path, out_dir, &verified, &total) != 0) {
        fprintf(stderr, "Error al descomprimir '%s'\n", archive_path);
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("Descompresion serial completa: %s -> %s\n", archive_path, out_dir);
    printf("Firmas verificadas: %d/%d (%.2f%% de salud)\n",
           verified, total, total > 0 ? (100.0 * verified / total) : 100.0);
    printf("Tiempo total: %.4f s\n", elapsed);
    return 0;
}
