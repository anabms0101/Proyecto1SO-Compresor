#include "archive.h"
#include "fileutils.h"
#include "entry_codec.h"
#include "binio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

/* Escribe 'len' bytes desde 'buf' al descriptor 'fd', reintentando ante
 * escrituras parciales (write() puede devolver menos de lo pedido). */
static int write_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* Lee todo lo que llegue por 'fd' (hasta EOF) y lo vuelca directo al
 * archivo de salida ya abierto. Esta es la comunicacion hijo->padre
 * (IPC via pipe): cada hijo comprime su rango de archivos y manda las
 * entradas ya serializadas por el pipe; el padre las escribe en orden
 * al .hzip final a medida que las recibe. */
static int drain_pipe_to_file(int fd, FILE *out) {
    unsigned char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) return -1;
    }
    return (n < 0) ? -1 : 0;
}

/* --- Proceso hijo: comprime su rango [start, end) y manda cada entrada
 * ya serializada por el extremo de escritura del pipe. --- */
static void worker_run(const char *dir_path, FileList *files, int start, int end, int write_fd) {
    for (int i = start; i < end; i++) {
        EncodedEntry e;
        if (entry_encode(dir_path, files->paths[i], &e) != 0) {
            close(write_fd);
            _exit(1);
        }
        int rc = write_all(write_fd, e.buf, e.len);
        entry_encoded_free(&e);
        if (rc != 0) {
            close(write_fd);
            _exit(1);
        }
    }
    close(write_fd);
    _exit(0);
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
        printf("RESULT ok=0\n");
        return 1;
    }

    if (num_workers > files.count) num_workers = files.count;

    /* Repartir los archivos en rangos contiguos entre los procesos */
    int base = files.count / num_workers;
    int extra = files.count % num_workers;

    int pipes[64][2];
    pid_t pids[64];
    int start = 0;

    for (int w = 0; w < num_workers; w++) {
        int count = base + (w < extra ? 1 : 0);
        int end = start + count;

        if (pipe(pipes[w]) != 0) {
            perror("pipe");
            file_list_free(&files);
            printf("RESULT ok=0\n");
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            file_list_free(&files);
            printf("RESULT ok=0\n");
            return 1;
        } else if (pid == 0) {
            /* --- Proceso hijo --- */
            close(pipes[w][0]); /* el hijo solo escribe */
            worker_run(dir_path, &files, start, end, pipes[w][1]);
            /* worker_run llama _exit, no regresa */
        } else {
            /* --- Proceso padre --- */
            close(pipes[w][1]); /* el padre solo lee */
            pids[w] = pid;
        }

        start = end;
    }

    /* El padre escribe la cabecera y luego drena cada pipe EN ORDEN,
     * volcando las entradas ya serializadas directo al .hzip final.
     * Los hijos corren en paralelo; si uno produce mas de lo que cabe
     * en el buffer del pipe (64 KB tipico en Linux) antes de que el
     * padre llegue a leerlo, simplemente se bloquea en write() hasta
     * que el padre lo drene -- no hay riesgo de interbloqueo porque
     * ningun hijo espera a otro hijo. */
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "No se pudo crear '%s'\n", out_path);
        for (int w = 0; w < num_workers; w++) close(pipes[w][0]);
        file_list_free(&files);
        printf("RESULT ok=0\n");
        return 1;
    }

    fwrite(ARCHIVE_MAGIC, 1, 4, out);
    write_u32(out, (uint32_t)files.count);

    int io_fail = 0;
    for (int w = 0; w < num_workers; w++) {
        if (drain_pipe_to_file(pipes[w][0], out) != 0) io_fail = 1;
        close(pipes[w][0]);
    }

    fclose(out);

    /* Recien ahora esperamos a los hijos: ya se drenaron todos los
     * pipes, asi que ningun hijo puede seguir bloqueado en write(). */
    int child_fail = 0;
    for (int w = 0; w < num_workers; w++) {
        int status;
        waitpid(pids[w], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) child_fail = 1;
    }

    file_list_free(&files);

    if (io_fail || child_fail) {
        fprintf(stderr, "Error: uno o mas procesos hijos fallaron al comprimir\n");
        remove(out_path);
        printf("RESULT ok=0\n");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    uint64_t original_size = directory_total_size(dir_path, recursive);
    uint64_t compressed_size = file_size_bytes(out_path);

    printf("Compresion paralela (fork + pipes) completa: %s -> %s\n", dir_path, out_path);
    printf("Procesos utilizados: %d\n", num_workers);
    printf("Tiempo total: %.4f s\n", elapsed);
    printf("Tamano original: %llu bytes\n", (unsigned long long)original_size);
    printf("Tamano comprimido: %llu bytes\n", (unsigned long long)compressed_size);
    if (original_size > 0) {
        printf("Radio de compresion: %.2f%%\n",
               100.0 * (double)compressed_size / (double)original_size);
    }

    printf("RESULT ok=1 elapsed=%.6f original_size=%llu compressed_size=%llu workers=%d\n",
           elapsed, (unsigned long long)original_size, (unsigned long long)compressed_size, num_workers);

    return 0;
}
