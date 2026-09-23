# Proyecto Compresor Huffman — Sistemas Operativos
 
## Compilar y probar
Antes de iniciar, se debe descomprimir el archivo .zip de la descarga de github:
```bash
#Primero debe verificar que este en usuario root
whoami
#y si no esta en root:
su -
#En un momento le preguntara si continuar, ahí se pone "s" o "y" para confirmar.
 
#0. Instalar dependencias
sudo apt update
sudo apt install build-essential libgtk-4-dev
 
#se sale del usuario root
exit
 
#1. Abrir la carpeta con el proyecto
cd Downloads
cd Proyecto1SO-Compresor-main
 
#2. Compilar
make clean
make all
./bin/gui_comparador
```
 
## Probar cada versión individualmente
 
Los 6 binarios quedan en `bin/` después de `make all`. Todos siguen la
misma convención: primero el origen, después el destino, y opcionalmente
la cantidad de procesos/hilos al final (si se omite, usan 4 por defecto).
No hay un límite fijo para esa cantidad — se puede pedir 1, 100 o
cualquier otro número que el sistema operativo pueda darte (cada
programa reserva en memoria dinámica exactamente lo que necesita).
 
### 1. Versión serial
 
```bash
./bin/compresor_serial ~/data ~/salida_serial.hzip --recursivo
./bin/descompresor_serial ~/salida_serial.hzip ~/destino_serial
diff -rq ~/data ~/destino_serial && echo "SERIAL: OK, idéntico"
```
 
### 2. Versión paralela con `fork()` + IPC (pipes)
 
El último argumento es la cantidad de **procesos**:
 
```bash
./bin/compressor_parallel ~/data ~/salida_fork.hzip --recursivo 4
./bin/decompressor_parallel ~/salida_fork.hzip ~/destino_fork 4
diff -rq ~/data ~/destino_fork && echo "FORK: OK, idéntico"
```
 
Se puede probar con cualquier N, por ejemplo 1, 16 o 100:
 
```bash
./bin/compressor_parallel ~/data ~/salida_fork_100.hzip --recursivo 100
```
 
**Si el directorio NO es recursivo**, hay que dejar el lugar del flag vacío
(`""`) para que el número de procesos caiga en la posición correcta:
 
```bash
./bin/compressor_parallel ~/data ~/salida_fork.hzip "" 4
```
 
### 3. Versión concurrente con `pthreads` + memoria compartida
 
Mismo formato de argumentos, pero el último número es la cantidad de
**hilos**:
 
```bash
./bin/compressor_thread ~/data ~/salida_thread.hzip --recursivo 4
./bin/decompressor_thread ~/salida_thread.hzip ~/destino_thread 4
diff -rq ~/data ~/destino_thread && echo "THREAD: OK, idéntico"
```
 
### 4. Confirmar que las 3 versiones dan exactamente el mismo resultado
 
```bash
md5sum ~/salida_serial.hzip ~/salida_fork.hzip ~/salida_thread.hzip
```
 
Los tres MD5 deberían ser idénticos, sin importar con cuántos
procesos/hilos se haya comprimido cada uno.
 
### Resumen rápido de los 6 binarios
 
```bash
# Version secuencial
./bin/compresor_serial <directorio_origen> <salida.hzip> [--recursivo]
./bin/descompresor_serial <salida.hzip> <directorio_destino>
 
# Version paralela con fork() + IPC (pipes)
./bin/compressor_parallel <directorio_origen> <salida.hzip> [--recursivo] [N_procesos]
./bin/decompressor_parallel <salida.hzip> <directorio_destino> [N_procesos]
 
# Version concurrente con pthreads + memoria compartida
./bin/compressor_thread <directorio_origen> <salida.hzip> [--recursivo] [N_hilos]
./bin/decompressor_thread <salida.hzip> <directorio_destino> [N_hilos]
 
# Interfaz grafica (compara las 3 versiones automaticamente)
./bin/gui_comparador
```
 
## La GUI
 
**Requisito previo:** los headers de desarrollo de GTK4 no vienen
instalados por defecto ni siquiera con GNOME instalado — hay que
agregarlos antes de compilar:
 
```bash
sudo apt install libgtk-4-dev
```
 
**Compilar y correr:**
 
```bash
make gui              # compila serial+fork+thread (si falta) y la GUI
./bin/gui_comparador
```

**Decision de diseno importante:** la GUI **no reimplementa** la logica
de `fork()` ni de `pthread()` dentro de su propio proceso. En cambio,
invoca los 6 binarios ya compilados (`compresor_serial`,
`compressor_parallel`, `compressor_thread` y sus 3 descompresores) como
subprocesos con `g_spawn_sync()`, y lee una linea `RESULT ...` que cada
programa imprime al final de su ejecucion para extraer las metricas
exactas (tiempo, tamanos, firmas verificadas).

Esto no es solo por simplicidad — es evitar un problema real y sutil de
sistemas operativos: **mezclar `fork()` con un proceso multi-hilo es
peligroso.** GTK usa hilos internamente (por ejemplo para I/O
asincrono), y si uno de esos hilos tiene tomado el *lock* interno del
`malloc()` de glibc justo en el instante en que otro hilo llama a
`fork()`, el proceso hijo hereda ese lock ya tomado — pero el hilo que
lo tenia **no existe** en el hijo (POSIX: `fork()` solo duplica el hilo
que lo invoco), asi que el hijo se queda esperando un lock que jamas se
va a liberar, y se cuelga en su primer `malloc()`. Al invocar los
binarios via `g_spawn` (que internamente hace `fork()`+`exec()`), el
hijo arranca como un proceso nuevo de un solo hilo antes de correr
nuestra logica de compresion — el `exec()` resetea la imagen de memoria
por completo, asi que este riesgo desaparece. Vale la pena mencionar
esta decision en la seccion de conclusiones del informe.

Para que la GUI pueda parsear los resultados, cada uno de los 6
programas de consola imprime, como ultima linea de su salida, algo asi:

```
RESULT ok=1 elapsed=0.001234 original_size=122012 compressed_size=66439
RESULT ok=1 elapsed=0.000987 verified=13 total=13
```

(la primera forma la usan los 3 compresores, la segunda los 3
descompresores). Si algo falla, imprimen `RESULT ok=0` y la GUI lo
marca como error en la tabla en vez de mostrar numeros inventados.

**Flujo de la interfaz:**
1. El usuario elige un directorio (`GtkFileDialog`, selector de carpetas
   nativo de GTK4) y opcionalmente tilda "incluir subdirectorios" y
   ajusta cuantos procesos/hilos usar.
2. Al presionar "Comprimir y descomprimir con las 3 versiones", un hilo
   de fondo (`GThread`, para no congelar la ventana) corre, en orden:
   `compresor_serial` → `compressor_parallel` → `compressor_thread` →
   `descompresor_serial` → `decompressor_parallel` → `decompressor_thread`,
   cada uno sobre un `.hzip` propio dentro de un directorio temporal
   (`g_dir_make_tmp`, no se toca el directorio original del usuario).
3. Los resultados se vuelcan a una tabla comparativa con las 8 metricas
   pedidas por el enunciado (salud, tiempo compresor, tiempo
   descompresor, % aceleracion del compresor y del descompresor
   respecto a la corrida serial, tamano original, tamano comprimido,
   radio de compresion) — una fila por version.
4. Un panel colapsable ("Ver salida detallada") muestra el log completo
   de las 6 corridas, util para depurar o para las capturas del informe..

## Correcciones aplicadas en esta revisión

1. **Typo en el `Makefile`** (`$@ $` en vez de `$@ $<` en la regla de los
   `.o` comunes): el proyecto no compilaba. Corregido.

2. **`compressor_parallel.c` (fork) ahora usa `pipe()` en vez de archivos
   temporales en `/tmp`.** Cada hijo comprime su rango de archivos y manda
   cada entrada ya serializada por el extremo de escritura de su pipe; el
   padre drena cada pipe **en orden** directo al `.hzip` final a medida
   que va llegando. Así la comunicación hijo→padre es una estrategia de
   IPC real (igual que ya usaba `decompressor_parallel.c`), y el reporte
   queda consistente entre compresor y descompresor. Probado con archivos
   de hasta 2 MB (muy por encima de los ~64 KB del buffer típico de un
   pipe en Linux) sin interbloqueo: el padre no espera a que todos los
   hijos terminen antes de empezar a leer, así que un hijo que llena su
   pipe simplemente se bloquea en `write()` hasta que el padre le toca
   drenarlo — nunca hay una espera circular entre procesos.

3. **Deduplicación de código entre fork y thread.** Antes, `write_u32` /
   `write_u64` / `read_u32` / `read_u64`, la lógica de "comprimir un
   archivo a un buffer en memoria" y la de "leer y verificar una entrada"
   estaban copiadas y pegadas en `src/fork/*.c` y `src/thread/*.c`. Ahora
   viven en un solo lugar, reutilizado por las 3 versiones:
   - `include/binio.h` + `src/common/binio.c` — lectura/escritura binaria
     little-endian (`write_u32`, `write_u64`, `read_u32`, `read_u64`).
   - `include/entry_codec.h` + `src/common/entry_codec.c` —
     `entry_encode()` (comprime un archivo a un buffer en memoria) y
     `entry_extract_one()` (lee y verifica una entrada desde un `FILE*`
     ya posicionado).
   - `archive_build_index()` (nueva función en `archive.h`/`archive.c`) —
     escanea el `.hzip` y devuelve los offsets de cada entrada, para que
     fork y thread puedan repartir el trabajo de descompresión sin
     duplicar ese escaneo.

   Los 4 archivos de `src/fork/` y `src/thread/` quedaron mucho más
   cortos: solo tienen la lógica específica de *cómo reparten el
   trabajo* (fork+pipes vs. cola compartida+mutex), no la lógica de
   formato del `.hzip`.

   Verificado de nuevo extremo a extremo: los `.hzip` generados por
   serial, fork y thread siguen siendo **byte a byte idénticos** (mismo
   MD5) sobre el mismo directorio de prueba.

## Arquitectura
 
```
include/            Headers compartidos por TODAS las versiones
  md5.h              MD5 (RFC 1321), implementado desde la especificación
  huffman.h          Compresión/descompresión Huffman en memoria
  archive.h          Formato contenedor .hzip (múltiples archivos + metadatos)
  fileutils.h        Listar directorios, leer/escribir archivos, mkdir -p
  binio.h            Lectura/escritura binaria little-endian compartida
  entry_codec.h       Comprimir/leer una entrada del .hzip (usado por fork y thread)
 
src/common/          Implementación de los headers de arriba (se reutiliza
                     tal cual en las 3 versiones: serial, fork, pthread)
 
src/serial/          Fase 1: compresor_serial.c / descompresor_serial.c
src/fork/            Fase 2: compressor_parallel.c / decompressor_parallel.c
                     — reparte los archivos en N procesos con fork(),
                     comunicación con el padre por pipe (IPC)
src/thread/          Fase 3: compressor_thread.c / decompressor_thread.c
                     — pool de N hilos con pthreads, cola de trabajo
                     compartida protegida con mutex
src/gui/             Fase 4: interfaz gráfica (GTK4) que invoca los 6
                     binarios como subprocesos y arma la tabla comparativa
 
data/                Aquí van los 100 archivos .txt descargados del top de
                     Project Gutenberg (aparece como data_de_prueba en este repo)
 
scripts/             descargar_corpus.sh (baja el top 100 de Gutenberg) y
                     benchmark_n.sh (mide tiempos variando N procesos/hilos)
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

Este mismo formato/API (`archive_compress_directory`, `archive_extract`)
lo van a llamar las 3 versiones (serial, fork, pthread) — lo que cambia
entre ellas es *cómo* se reparte el trabajo de comprimir/descomprimir
cada archivo, no el formato del contenedor.
