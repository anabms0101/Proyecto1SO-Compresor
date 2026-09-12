#include "archive.h"
#include "fileutils.h"
#include "huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

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

/* --- Cola de trabajo compartida entre hilos ---
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

/* Resultado de comprimir UN archivo: queda en memoria (open_memstream)
 * listo para volcarse tal cual al .hzip final. */
typedef struct {
    char *buf;
    size_t len;
} Entry;

/* Comprime un archivo y escribe su "entrada" binaria a un buffer en
 * memoria (mismo formato que usa la version serial). */
static int compress_to_memory(const char *dir_path, const char *rel_path, Entry *out) {
    char full_path[4096];
    join_path(full_path, sizeof(full_path), dir_path, rel_path);

    unsigned char *content = NULL;
    uint64_t content_len = 0;
    if (read_entire_file(full_path, &content, &content_len) != 0) return -1;

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

    char *membuf = NULL;
    size_t memsize = 0;
    FILE *mem = open_memstream(&membuf, &memsize);
    if (!mem) {
        huffman_result_free(&hr);
        free(content);
        return -1;
    }

    uint32_t name_len = (uint32_t)strlen(rel_path);
    write_u32(mem, name_len);
    fwrite(rel_path, 1, name_len, mem);
    write_u64(mem, content_len);
    fwrite(md5, 1, MD5_DIGEST_SIZE, mem);
    for (int k = 0; k < 256; k++) write_u64(mem, hr.freq[k]);
    write_u64(mem, hr.bit_count);
    write_u64(mem, hr.data_len);
    if (hr.data_len > 0) fwrite(hr.data, 1, hr.data_len, mem);

    fclose(mem); /* actualiza membuf/memsize con el contenido final */

    out->buf = membuf;
    out->len = memsize;

    huffman_result_free(&hr);
    free(content);
    return 0;
}

typedef struct {
    const char *dir_path;
    WorkQueue *queue;
    Entry *results; /* arreglo compartido: cada hilo escribe SOLO en su indice */
} ThreadArg;

static void *worker_thread(void *arg_) {
    ThreadArg *arg = (ThreadArg *)arg_;

    int idx;
    while ((idx = queue_take(arg->queue)) != -1) {
        Entry e;
        if (compress_to_memory(arg->dir_path, arg->queue->files->paths[idx], &e) != 0) {
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
    if (num_threads > 64) num_threads = 64;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    FileList files;
    list_directory_files(dir_path, recursive, &files);

    if (files.count == 0) {
        fprintf(stderr, "No hay archivos para comprimir en '%s'\n", dir_path);
        file_list_free(&files);
        return 1;
    }

    if (num_threads > files.count) num_threads = files.count;

    Entry *results = calloc((size_t)files.count, sizeof(Entry));

    WorkQueue queue;
    queue.files = &files;
    queue.next_index = 0;
    queue.error_flag = 0;
    pthread_mutex_init(&queue.lock, NULL);

    pthread_t threads[64];
    ThreadArg args[64];

    for (int t = 0; t < num_threads; t++) {
        args[t].dir_path = dir_path;
        args[t].queue = &queue;
        args[t].results = results;
        pthread_create(&threads[t], NULL, worker_thread, &args[t]);
    }

    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }

    pthread_mutex_destroy(&queue.lock);

    if (queue.error_flag) {
        fprintf(stderr, "Error al comprimir uno o mas archivos\n");
        for (int i = 0; i < files.count; i++) free(results[i].buf);
        free(results);
        file_list_free(&files);
        return 1;
    }

    /* Solo el hilo principal escribe el archivo final, en orden.
     * No hace falta mutex de escritura: para este punto todos los
     * hilos trabajadores ya terminaron (pthread_join). */
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "No se pudo crear '%s'\n", out_path);
        for (int i = 0; i < files.count; i++) free(results[i].buf);
        free(results);
        file_list_free(&files);
        return 1;
    }

    fwrite(ARCHIVE_MAGIC, 1, 4, out);
    write_u32(out, (uint32_t)files.count);

    for (int i = 0; i < files.count; i++) {
        fwrite(results[i].buf, 1, results[i].len, out);
        free(results[i].buf);
    }

    fclose(out);
    free(results);
    file_list_free(&files);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("Compresion concurrente (pthreads) completa: %s -> %s\n", dir_path, out_path);
    printf("Hilos utilizados: %d\n", num_threads);
    printf("Tiempo total: %.4f s\n", elapsed);

    return 0;
}
