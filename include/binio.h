#ifndef BINIO_H
#define BINIO_H

#include <stdio.h>
#include <stdint.h>

/* Helpers de lectura/escritura binaria portable (little-endian explicito,
 * byte a byte, para no depender del endianness de la maquina).
 * Usados por archive.c y por TODAS las versiones (serial, fork, thread)
 * para leer/escribir el formato .hzip, asi se evita tener 4 copias del
 * mismo codigo. */

void write_u32(FILE *f, uint32_t v);
void write_u64(FILE *f, uint64_t v);

/* Devuelven 0 en exito, -1 si no se pudo leer la cantidad esperada de bytes. */
int read_u32(FILE *f, uint32_t *v);
int read_u64(FILE *f, uint64_t *v);

#endif /* BINIO_H */
