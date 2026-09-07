#ifndef ARCHIVE_H
#define ARCHIVE_H

#include <stdint.h>
#include <stddef.h>
#include "md5.h"

#define ARCHIVE_MAGIC "HZP1"

/* Formato binario de un .hzip (todo en little-endian, escrito byte a byte
 * para no depender del endianness de la maquina):
 *
 *   char     magic[4]              "HZP1"
 *   uint32   num_files
 *   por cada archivo:
 *     uint32   name_len
 *     char     name[name_len]      (ruta relativa dentro del directorio)
 *     uint64   original_size
 *     uchar    md5[16]             MD5 del archivo ORIGINAL (pre-compresion)
 *     uint64   freq[256]           tabla de frecuencias (para reconstruir
 *                                  el arbol de Huffman al descomprimir)
 *     uint64   bit_count           bits significativos del bitstream
 *     uint64   compressed_bytes    bytes que ocupa el bitstream empaquetado
 *     uchar    data[compressed_bytes]
 */

/* Comprime todos los archivos regulares del directorio 'dir_path'
 * (no recursivo por defecto, ver 'recursive') en el archivo 'out_path'.
 * Devuelve 0 en exito, -1 en error. */
int archive_compress_directory(const char *dir_path, const char *out_path, int recursive);

/* Descomprime el archivo 'archive_path' en el directorio 'out_dir'
 * (se crea si no existe). Verifica el MD5 de cada archivo restaurado
 * contra el guardado en el contenedor.
 * 'verified_count' y 'total_count' quedan con el resultado para poder
 * calcular el "porcentaje de salud de la compresion".
 * Devuelve 0 en exito (incluso si alguna firma no coincide: eso se
 * reporta via verified_count/total_count, no como error fatal), -1 en
 * error de I/O. */
int archive_extract(const char *archive_path, const char *out_dir,
                     int *verified_count, int *total_count);

#endif /* ARCHIVE_H */
