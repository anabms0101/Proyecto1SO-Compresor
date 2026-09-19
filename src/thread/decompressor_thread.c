#include "archive.h"
#include "fileutils.h"
#include "entry_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* ---Cola de trabajo y contador compartido de verificados,
 * ambos protegidos por el mismo mutex.--- */
typedef struct {
    long *offsets;
    uint32_t total;
    uint32_t next_index;
    int total_verified; /* acumulador compartido */
    pthread_mutex_t lock;
} WorkQueue;

static int queue_take(WorkQueue *q) {
    int idx = -1;
    pthread_mutex_lock(&q->lock);
    if (q->next_index < q->total) {
        idx = (int)q->next_index;
        q->next_index++;
    }
    pthread_mutex_unlock(&q->lock);
    return idx;
}

static void queue_add_verified(WorkQueue *q, int count) {
    pthread_mutex_lock(&q->lock);
    q->total_verified += count;
    pthread_mutex_unlock(&q->lock);
}

typedef struct {
    const char *archive_path;
    const char *out_dir;
    WorkQueue *queue;
} ThreadArg;

static void *worker_thread(void *arg_) {
    ThreadArg *arg = (ThreadArg *)arg_;

    /* Cada hilo abre su PROPIO FILE* para el archivo de entrada, asi
     * evitamos compartir la posicion de lectura (fseek/fread) entre
     * hilos, que si necesitaria mutex. Escribir archivos distintos en
     * out_dir tampoco requiere sincronizacion: cada archivo tiene
     * nombre unico. */
    FILE *in = fopen(arg->archive_path, "rb");
    if (!in) return NULL;

    int local_verified = 0;
    int idx;
    while ((idx = queue_take(arg->queue)) != -1) {
        fseek(in, arg->queue->offsets[idx], SEEK_SET);
        int ok = 0;
        if (entry_extract_one(in, arg->out_dir, &ok) == 0 && ok) local_verified++;
    }

    fclose(in);
    queue_add_verified(arg->queue, local_verified);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <archivo.hzip> <directorio_destino> [num_hilos]\n", argv[0]);
        return 1;
    }

    const char *archive_path = argv[1];
    const char *out_dir = argv[2];
    int num_threads = (argc >= 4) ? atoi(argv[3]) : 4;
    if (num_threads < 1) num_threads = 1;
    /* Sin limite fijo */

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    long *offsets = NULL;
    uint32_t total = 0;
    if (archive_build_index(archive_path, &offsets, &total) != 0) {
        fprintf(stderr, "Error al leer '%s'\n", archive_path);
        printf("RESULT ok=0\n");
        return 1;
    }

    if (total == 0) {
        printf("Archivo vacio, nada que descomprimir.\n");
        printf("RESULT ok=1 elapsed=0.000000 verified=0 total=0\n");
        free(offsets);
        return 0;
    }

    make_dirs_recursive(out_dir);

    if ((uint32_t)num_threads > total) num_threads = (int)total;

    WorkQueue queue;
    queue.offsets = offsets;
    queue.total = total;
    queue.next_index = 0;
    queue.total_verified = 0;
    pthread_mutex_init(&queue.lock, NULL);

    pthread_t *threads = malloc(sizeof(pthread_t) * (size_t)num_threads);
    ThreadArg *args = malloc(sizeof(ThreadArg) * (size_t)num_threads);
    if (!threads || !args) {
        fprintf(stderr, "No hay memoria suficiente para %d hilos\n", num_threads);
        free(threads);
        free(args);
        pthread_mutex_destroy(&queue.lock);
        free(offsets);
        printf("RESULT ok=0\n");
        return 1;
    }

    for (int t = 0; t < num_threads; t++) {
        args[t].archive_path = archive_path;
        args[t].out_dir = out_dir;
        args[t].queue = &queue;
        pthread_create(&threads[t], NULL, worker_thread, &args[t]);
    }

    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }

    free(threads);
    free(args);
    pthread_mutex_destroy(&queue.lock);
    free(offsets);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("Descompresion concurrente (pthreads) completa: %s -> %s\n", archive_path, out_dir);
    printf("Hilos utilizados: %d\n", num_threads);
    printf("Firmas verificadas: %d/%u (%.2f%% de salud)\n",
           queue.total_verified, total, total > 0 ? (100.0 * queue.total_verified / total) : 100.0);
    printf("Tiempo total: %.4f s\n", elapsed);

    printf("RESULT ok=1 elapsed=%.6f verified=%d total=%u threads=%d\n",
           elapsed, queue.total_verified, total, num_threads);

    return 0;
}
