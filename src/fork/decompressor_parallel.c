#include "archive.h"
#include "fileutils.h"
#include "entry_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

/* El hijo procesa su rango y reporta cuantas firmas verifico via pipe
 * (IPC): un solo int con el conteo local. */
static void worker_run(const char *archive_path, const char *out_dir,
                        long *offsets, int start, int end, int write_fd) {
    FILE *in = fopen(archive_path, "rb");
    int verified = 0;

    if (in) {
        for (int i = start; i < end; i++) {
            fseek(in, offsets[i], SEEK_SET);
            int ok = 0;
            if (entry_extract_one(in, out_dir, &ok) == 0 && ok) verified++;
        }
        fclose(in);
    }

    if (write(write_fd, &verified, sizeof(int)) != sizeof(int)) {
        /* Poco que hacer si falla el pipe a esta altura; el padre lo
         * detecta porque read() no recibira el entero esperado. */
    }
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
    if (archive_build_index(archive_path, &offsets, &total) != 0) {
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
        if (read(pipes[w][0], &count, sizeof(int)) != sizeof(int)) count = 0;
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
