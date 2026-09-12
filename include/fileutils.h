#ifndef FILEUTILS_H
#define FILEUTILS_H

#include <stdint.h>
#include <stddef.h>

/* Lee un archivo completo en memoria. El caller debe hacer free(*out_buf).
 * Devuelve 0 en exito, -1 en error. */
int read_entire_file(const char *path, unsigned char **out_buf, uint64_t *out_len);

/* Escribe 'len' bytes de 'buf' en 'path', sobrescribiendo si existe.
 * Devuelve 0 en exito, -1 en error. */
int write_entire_file(const char *path, const unsigned char *buf, uint64_t len);

/* Crea 'path' y todos sus directorios padre si no existen (equivalente a
 * mkdir -p). Devuelve 0 en exito, -1 en error. */
int make_dirs_recursive(const char *path);

/* Lista de rutas relativas de archivos regulares dentro de 'dir_path'.
 * Si 'recursive' es distinto de 0, entra en subdirectorios. */
typedef struct {
    char **paths;   /* rutas relativas a dir_path, ej "sub/archivo.txt" */
    int count;
} FileList;

int list_directory_files(const char *dir_path, int recursive, FileList *out);
void file_list_free(FileList *list);

/* Construye 'dst' = 'dir' + "/" + 'name', con manejo de tamano de buffer. */
void join_path(char *dst, size_t dst_size, const char *dir, const char *name);

/* Suma el tamano en bytes de todos los archivos regulares dentro de
 * 'dir_path' (usa la misma logica de recorrido que list_directory_files,
 * asi que 'recursive' debe coincidir con el usado al comprimir). */
uint64_t directory_total_size(const char *dir_path, int recursive);

/* Tamano en bytes de un archivo. Devuelve 0 si no existe o hay error. */
uint64_t file_size_bytes(const char *path);

#endif /* FILEUTILS_H */
