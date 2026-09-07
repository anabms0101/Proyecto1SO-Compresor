#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stdint.h>
#include <stddef.h>

/* Resultado de comprimir un buffer en memoria con Huffman.
 * freq[256] queda con las frecuencias originales: son suficientes para que
 * el decodificador reconstruya el mismo arbol (no se serializa el arbol,
 * se reconstruye a partir de las frecuencias, que se guardan en el
 * contenedor .hzip junto a los metadatos de cada archivo). */
typedef struct {
    uint64_t freq[256];
    unsigned char *data;   /* bits empaquetados en bytes */
    uint64_t data_len;     /* bytes usados en data */
    uint64_t bit_count;    /* cantidad exacta de bits significativos */
} HuffmanResult;

/* Comprime 'input' (in_len bytes) con Huffman clasico.
 * Devuelve 0 en exito, -1 en error. El caller debe llamar
 * huffman_result_free() sobre 'out' cuando termine de usarlo. */
int huffman_compress(const unsigned char *input, uint64_t in_len, HuffmanResult *out);

/* Descomprime usando la tabla de frecuencias 'freq' (misma que se uso al
 * comprimir) y el bitstream 'data' con 'bit_count' bits significativos.
 * Escribe exactamente 'original_len' bytes en 'output' (ya reservado por
 * el caller). Devuelve 0 en exito, -1 en error. */
int huffman_decompress(const uint64_t freq[256],
                        const unsigned char *data,
                        uint64_t bit_count,
                        unsigned char *output,
                        uint64_t original_len);

void huffman_result_free(HuffmanResult *r);

#endif /* HUFFMAN_H */
