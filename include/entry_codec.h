#ifndef ENTRY_CODEC_H
#define ENTRY_CODEC_H

#include <stdint.h>
#include <stdio.h>

/* Una "entrada" es el bloque binario de UN archivo dentro del .hzip:
 * name_len+name, original_size, md5, freq[256], bit_count,
 * compressed_bytes+data. Mismo formato que describe archive.h. */

typedef struct {
    char *buf;    /* buffer en memoria con la entrada ya serializada */
    size_t len;
} EncodedEntry;

/* Comprime el archivo 'dir_path/rel_path' y serializa su entrada completa
 * a un buffer en memoria (usando open_memstream). El caller debe hacer
 * free(out->buf). Devuelve 0 en exito, -1 en error. */
int entry_encode(const char *dir_path, const char *rel_path, EncodedEntry *out);

void entry_encoded_free(EncodedEntry *e);

/* Lee y descomprime UNA entrada desde 'in' (que debe estar posicionado
 * justo al inicio de la entrada, es decir, antes de su 'name_len').
 * Escribe el archivo restaurado dentro de 'out_dir' y verifica su MD5.
 * '*ok' queda en 1 si la firma coincide, 0 si no. Devuelve 0 si pudo
 * leer/procesar la entrada (haya coincidido el MD5 o no), -1 en error
 * de formato/E-S. */
int entry_extract_one(FILE *in, const char *out_dir, int *ok);

#endif /* ENTRY_CODEC_H */
