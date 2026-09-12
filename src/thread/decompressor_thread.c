#include "archive.h"
#include "fileutils.h"
#include "huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

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
 * para poder repartir el trabajo entre hilos sin descomprimir aun. */
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
        fseek(in, 256 * 8, SEEK_CUR);

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

/* --- Cola de trabajo y contador compartido de verificados,
 * ambos protegidos por el mismo mutex. --- */
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
        if (extract_one(in, arg->out_dir, &ok) == 0 && ok) local_verified++;
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
    if (num_threads > 64) num_threads = 64;

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

    if ((uint32_t)num_threads > total) num_threads = (int)total;

    WorkQueue queue;
    queue.offsets = offsets;
    queue.total = total;
    queue.next_index = 0;
    queue.total_verified = 0;
    pthread_mutex_init(&queue.lock, NULL);

    pthread_t threads[64];
    ThreadArg args[64];

    for (int t = 0; t < num_threads; t++) {
        args[t].archive_path = archive_path;
        args[t].out_dir = out_dir;
        args[t].queue = &queue;
        pthread_create(&threads[t], NULL, worker_thread, &args[t]);
    }

    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }

    pthread_mutex_destroy(&queue.lock);
    free(offsets);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("Descompresion concurrente (pthreads) completa: %s -> %s\n", archive_path, out_dir);
    printf("Hilos utilizados: %d\n", num_threads);
    printf("Firmas verificadas: %d/%u (%.2f%% de salud)\n",
           queue.total_verified, total, total > 0 ? (100.0 * queue.total_verified / total) : 100.0);
    printf("Tiempo total: %.4f s\n", elapsed);

    return 0;
}
