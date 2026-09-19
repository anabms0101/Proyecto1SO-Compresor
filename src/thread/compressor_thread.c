#include "archive.h"
#include "fileutils.h"
#include "entry_codec.h"
#include "binio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* ---Cola de trabajo compartida entre hilos---
 * Un solo entero (next_index) protegido por mutex: cada hilo lo lee y
 * lo incrementa de forma atomica para "tomar" el siguiente archivo. */
typedef struct {
    FileList *files;
    int next_index;
    pthread_mutex_t lock;
    int error_flag; /* tambien protegido por 'lock' */
} WorkQueue;

static int queue_take(WorkQueue *q) {
    int idx = -1;
    pthread_mutex_lock(&q->lock);
    if (q->next_index < q->files->count) {
        idx = q->next_index;
        q->next_index++;
    }
    pthread_mutex_unlock(&q->lock);
    return idx;
}

static void queue_mark_error(WorkQueue *q) {
    pthread_mutex_lock(&q->lock);
    q->error_flag = 1;
    pthread_mutex_unlock(&q->lock);
}

typedef struct {
    const char *dir_path;
    WorkQueue *queue;
    EncodedEntry *results; /* arreglo compartido: cada hilo escribe SOLO en su indice */
} ThreadArg;

static void *worker_thread(void *arg_) {
    ThreadArg *arg = (ThreadArg *)arg_;

    int idx;
    while ((idx = queue_take(arg->queue)) != -1) {
        EncodedEntry e;
        if (entry_encode(arg->dir_path, arg->queue->files->paths[idx], &e) != 0) {
            queue_mark_error(arg->queue);
            continue;
        }
        /* Escritura segura sin mutex: cada 'idx' es unico entre hilos,
         * asi que nunca dos hilos escriben la misma celda del arreglo. */
        arg->results[idx] = e;
    }

    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "Uso: %s <directorio_origen> <archivo_salida.hzip> [--recursivo] [num_hilos]\n",
            argv[0]);
        return 1;
    }

    const char *dir_path = argv[1];
    const char *out_path = argv[2];
    int recursive = (argc >= 4 && strcmp(argv[3], "--recursivo") == 0);
    int num_threads = (argc >= 5) ? atoi(argv[4]) : 4;
    if (num_threads < 1) num_threads = 1;
    /* Sin limite fijo */

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

    if (num_threads > files.count) num_threads = files.count;

    EncodedEntry *results = calloc((size_t)files.count, sizeof(EncodedEntry));

    WorkQueue queue;
    queue.files = &files;
    queue.next_index = 0;
    queue.error_flag = 0;
    pthread_mutex_init(&queue.lock, NULL);

    pthread_t *threads = malloc(sizeof(pthread_t) * (size_t)num_threads);
    ThreadArg *args = malloc(sizeof(ThreadArg) * (size_t)num_threads);
    if (!threads || !args) {
        fprintf(stderr, "No hay memoria suficiente para %d hilos\n", num_threads);
        free(threads);
        free(args);
        free(results);
        pthread_mutex_destroy(&queue.lock);
        file_list_free(&files);
        printf("RESULT ok=0\n");
        return 1;
    }

    for (int t = 0; t < num_threads; t++) {
        args[t].dir_path = dir_path;
        args[t].queue = &queue;
        args[t].results = results;
        pthread_create(&threads[t], NULL, worker_thread, &args[t]);
    }

    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }

    free(threads);
    free(args);
    pthread_mutex_destroy(&queue.lock);

    if (queue.error_flag) {
        fprintf(stderr, "Error al comprimir uno o mas archivos\n");
        for (int i = 0; i < files.count; i++) entry_encoded_free(&results[i]);
        free(results);
        file_list_free(&files);
        printf("RESULT ok=0\n");
        return 1;
    }

    /* Solo el hilo principal escribe el archivo final, en orden.
     * No hace falta mutex de escritura: para este punto todos los
     * hilos trabajadores ya terminaron (pthread_join). */
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "No se pudo crear '%s'\n", out_path);
        for (int i = 0; i < files.count; i++) entry_encoded_free(&results[i]);
        free(results);
        file_list_free(&files);
        printf("RESULT ok=0\n");
        return 1;
    }

    fwrite(ARCHIVE_MAGIC, 1, 4, out);
    write_u32(out, (uint32_t)files.count);

    for (int i = 0; i < files.count; i++) {
        fwrite(results[i].buf, 1, results[i].len, out);
        entry_encoded_free(&results[i]);
    }

    fclose(out);
    free(results);
    file_list_free(&files);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    uint64_t original_size = directory_total_size(dir_path, recursive);
    uint64_t compressed_size = file_size_bytes(out_path);

    printf("Compresion concurrente (pthreads) completa: %s -> %s\n", dir_path, out_path);
    printf("Hilos utilizados: %d\n", num_threads);
    printf("Tiempo total: %.4f s\n", elapsed);
    printf("Tamano original: %llu bytes\n", (unsigned long long)original_size);
    printf("Tamano comprimido: %llu bytes\n", (unsigned long long)compressed_size);
    if (original_size > 0) {
        printf("Radio de compresion: %.2f%%\n",
               100.0 * (double)compressed_size / (double)original_size);
    }

    printf("RESULT ok=1 elapsed=%.6f original_size=%llu compressed_size=%llu threads=%d\n",
           elapsed, (unsigned long long)original_size, (unsigned long long)compressed_size, num_threads);

    return 0;
}
