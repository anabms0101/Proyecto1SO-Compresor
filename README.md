# Proyecto Compresor Huffman — Sistemas Operativos

## Estado actual

**Fase 1 (serial) — completa y probada.**
Los módulos comunes (MD5, Huffman, formato de archivo, utilidades de
directorio) están terminados y son la base que reutilizarán las versiones
paralela (fork) y concurrente (pthreads). El compresor/descompresor serial
compila limpio y fue probado con:
- Texto normal, texto con acentos/UTF-8
- Un archivo vacío
- Un archivo de un solo carácter repetido (caso especial del árbol de Huffman)
- Un archivo binario aleatorio
- Un subdirectorio (modo `--recursivo`)

Resultado: 5/5 firmas MD5 verificadas, `diff -rq` entre directorio original
y descomprimido sin diferencias.

Fase 2 — compresor/descompresor paralelo con `fork()` + IPC
Fase 3 — compresor/descompresor concurrente con `pthread()` + memoria compartida
Fase 4 — GUI (GTK) con tabla comparativa de estadísticas
Reporte LaTeX

## Arquitectura

```
include/            Headers compartidos por TODAS las versiones
  md5.h              MD5 (RFC 1321), implementado desde la especificación
  huffman.h          Compresión/descompresión Huffman en memoria
  archive.h          Formato contenedor .hzip (múltiples archivos + metadatos)
  fileutils.h        Listar directorios, leer/escribir archivos, mkdir -p

src/common/          Implementación de los headers de arriba (se reutiliza
                     tal cual en las 3 versiones: serial, fork, pthread)

src/serial/          Fase 1: compresor_serial.c / decompressor_serial.c
src/parallel_fork/   Fase 2: un proceso hijo por archivo (o por lote),
                     comunicación con el padre por pipe
src/parallel_thread/ Fase 3: un pool de threads, memoria compartida entre
                     threads del mismo proceso + mutex para sincronizar
src/gui/             Fase 4: interfaz gráfica (GTK) que invoca las 3
                     versiones y arma la tabla comparativa

data/                Aquí van los 100 archivos .txt descargados del top de
                     Project Gutenberg (no se incluyen en este repo)
```

## El formato `.hzip`

En vez de comprimir cada archivo del directorio por separado, todo el
directorio se empaqueta en **un solo archivo `.hzip`** con esta estructura:

```
"HZP1"                    magic number
uint32 num_files
por cada archivo:
  uint32 name_len + char name[name_len]     ruta relativa (soporta subdirs)
  uint64 original_size
  uchar  md5[16]                            MD5 del archivo ORIGINAL
  uint64 freq[256]                          tabla de frecuencias de bytes
  uint64 bit_count                          bits significativos del stream
  uint64 compressed_bytes + data[...]       bitstream de Huffman empaquetado
```

**Decisión de diseño clave:** en vez de serializar el árbol de Huffman
(código canónico, etc.), se guarda la tabla de 256 frecuencias. El
decodificador reconstruye el **mismo árbol** determinísticamente a partir
de las frecuencias (mismo algoritmo de construcción = mismo árbol). Es
más simple de implementar y depurar, y el costo extra (256 × 8 bytes =
2 KB por archivo) es insignificante frente al tamaño de los libros de
Gutenberg. Esto es importante para el reporte: hay que **explicar y
justificar** esta decisión en la sección de "Descripción de la
implementación del algoritmo de Huffman".

Este mismo formato/API (`archive_compress_directory`, `archive_extract`)
lo van a llamar las 3 versiones (serial, fork, pthread) — lo que cambia
entre ellas es *cómo* se reparte el trabajo de comprimir/descomprimir
cada archivo, no el formato del contenedor.

## Compilar y probar

```bash
make serial          # compila bin/compresor_serial y bin/descompresor_serial

./bin/compresor_serial <directorio_origen> <salida.hzip> [--recursivo]
./bin/descompresor_serial <salida.hzip> <directorio_destino>
```

## Próximos pasos sugeridos (en orden)

1. **Fase 2 (fork + IPC):** cada proceso hijo comprime uno o varios
   archivos y devuelve `(nombre, freq[256], bit_count, data)` al padre.
   Como `fork()` copia la memoria pero no la comparte, hay que decidir el
   mecanismo de IPC: la opción más simple y robusta es que cada hijo
   escriba su resultado ya serializado a un **pipe** hacia el padre (o a
   un archivo temporal), y el padre sea el único que escribe el `.hzip`
   final — así se evita sincronizar escrituras concurrentes al mismo
   archivo.

2. **Fase 3 (pthreads + memoria compartida):** un pool de N threads toma
   archivos de una cola compartida (protegida con `pthread_mutex_t`), cada
   uno comprime en un buffer propio, y el hilo principal escribe el
   `.hzip` cuando todos terminan (o usa un mutex para escritura ordenada).
   Aquí "memoria compartida" es más directa que en fork, porque los
   threads ya comparten el espacio de direcciones del proceso — hay que
   dejar claro en el informe cómo se sincroniza el acceso a la cola y a la
   escritura del archivo de salida.

3. **Fase 4 (GUI):** reutiliza las funciones de `archive.h` (o invoca los
   3 binarios como subprocesos) y arma la tabla comparativa con las
   métricas pedidas: salud, tiempos, aceleración, tamaños, radio de
   compresión.

4. **Descarga del corpus:** bajar el top 100 (30 días) de Project
   Gutenberg, filtrando solo los que tengan formato de texto plano (`.txt`),
   y guardarlos en `data/`.

5. **Reporte LaTeX:** con la plantilla del curso, secciones exactas del
   enunciado, y cuidado especial con las citas (toda función/algoritmo
   explicado que no sea de autoría propia —p. ej. `pipe()`, `fork()`,
   Huffman— debe citarse en formato APA 7).

¿Seguimos con la Fase 2 (fork + IPC) en la próxima sesión?
