#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
 
static void mkdir_p(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
 
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}
 
static bool es_directorio(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}
 
static const char *nombre_base(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
 

typedef struct {
    uint8_t tipo;       /* 0 = archivo, 1 = carpeta */
    char *ruta;         /* ruta relativa, ej "sub/archivo.txt" */
    uint64_t tam;       /* tamaño del contenido (0 si es carpeta) */
    uint8_t *datos;     /* contenido del archivo (NULL si es carpeta) */
} Entrada;
 
typedef struct {
    Entrada *items;
    size_t cuenta;
    size_t capacidad;
} ListaEntradas;
 
static void lista_init(ListaEntradas *l) {
    l->items = NULL;
    l->cuenta = 0;
    l->capacidad = 0;
}
 
static Entrada *lista_agregar(ListaEntradas *l) {
    if (l->cuenta == l->capacidad) {
        l->capacidad = l->capacidad ? l->capacidad * 2 : 16;
        l->items = realloc(l->items, l->capacidad * sizeof(Entrada));
    }
    return &l->items[l->cuenta++];
}
 
/* Lee un archivo completo a memoria */
static uint8_t *leer_archivo(const char *path, uint64_t *tam_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long tam = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(tam > 0 ? tam : 1);
    if (tam > 0) fread(buf, 1, tam, f);
    fclose(f);
    *tam_out = (uint64_t)tam;
    return buf;
}
 
/* Recorre recursivamente una carpeta y llena la lista de entradas.
   "raiz" es la carpeta original; "rel" es la ruta relativa actual. */
static void recorrer(const char *raiz, const char *rel, ListaEntradas *lista) {
    char completo[4096];
    if (rel[0] == '\0')
        snprintf(completo, sizeof(completo), "%s", raiz);
    else
        snprintf(completo, sizeof(completo), "%s/%s", raiz, rel);
 
    DIR *d = opendir(completo);
    if (!d) { perror(completo); exit(1); }
 
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
 
        char rel_hijo[4096];
        if (rel[0] == '\0')
            snprintf(rel_hijo, sizeof(rel_hijo), "%s", ent->d_name);
        else
            snprintf(rel_hijo, sizeof(rel_hijo), "%s/%s", rel, ent->d_name);
 
        char path_hijo[4096];
        snprintf(path_hijo, sizeof(path_hijo), "%s/%s", completo, ent->d_name);
 
        if (es_directorio(path_hijo)) {
            Entrada *e = lista_agregar(lista);
            e->tipo = 1;
            e->ruta = strdup(rel_hijo);
            e->tam = 0;
            e->datos = NULL;
            recorrer(raiz, rel_hijo, lista);
        } else {
            Entrada *e = lista_agregar(lista);
            e->tipo = 0;
            e->ruta = strdup(rel_hijo);
            e->datos = leer_archivo(path_hijo, &e->tam);
        }
    }
    closedir(d);
}
 
/* Serializa la lista de entradas en un solo buffer contiguo de bytes.
   Formato: [uint32 num_entradas] { [u8 tipo][u16 largo_ruta][ruta]
             (si tipo==0) [u64 tam][datos] } * num_entradas          */
static uint8_t *serializar(ListaEntradas *lista, uint64_t *tam_total_out) {
    uint64_t tam_total = 4; /* num_entradas */
    for (size_t i = 0; i < lista->cuenta; i++) {
        Entrada *e = &lista->items[i];
        tam_total += 1 + 2 + strlen(e->ruta);
        if (e->tipo == 0) tam_total += 8 + e->tam;
    }
 
    uint8_t *buf = malloc(tam_total);
    uint64_t pos = 0;
 
    uint32_t num = (uint32_t)lista->cuenta;
    memcpy(buf + pos, &num, 4); pos += 4;
 
    for (size_t i = 0; i < lista->cuenta; i++) {
        Entrada *e = &lista->items[i];
        buf[pos++] = e->tipo;
 
        uint16_t largo_ruta = (uint16_t)strlen(e->ruta);
        memcpy(buf + pos, &largo_ruta, 2); pos += 2;
        memcpy(buf + pos, e->ruta, largo_ruta); pos += largo_ruta;
 
        if (e->tipo == 0) {
            memcpy(buf + pos, &e->tam, 8); pos += 8;
            if (e->tam > 0) memcpy(buf + pos, e->datos, e->tam);
            pos += e->tam;
        }
    }
 
    *tam_total_out = tam_total;
    return buf;
}
 
/* Reconstruye archivos/carpetas en disco a partir del contenedor deserializado */
static void extraer(const uint8_t *buf, uint64_t tam_total, const char *destino) {
    mkdir_p(destino);
 
    uint64_t pos = 0;
    uint32_t num;
    memcpy(&num, buf + pos, 4); pos += 4;
 
    for (uint32_t i = 0; i < num; i++) {
        uint8_t tipo = buf[pos++];
 
        uint16_t largo_ruta;
        memcpy(&largo_ruta, buf + pos, 2); pos += 2;
 
        char ruta[4096];
        memcpy(ruta, buf + pos, largo_ruta);
        ruta[largo_ruta] = '\0';
        pos += largo_ruta;
 
        char path_final[4096];
        snprintf(path_final, sizeof(path_final), "%s/%s", destino, ruta);
 
        if (tipo == 1) {
            mkdir_p(path_final);
        } else {
            uint64_t tam;
            memcpy(&tam, buf + pos, 8); pos += 8;
 
            /* Asegurar que exista la carpeta padre del archivo */
            char copia[4096];
            snprintf(copia, sizeof(copia), "%s", path_final);
            char *ultimo_slash = strrchr(copia, '/');
            if (ultimo_slash) {
                *ultimo_slash = '\0';
                mkdir_p(copia);
            }
 
            FILE *f = fopen(path_final, "wb");
            if (!f) { perror(path_final); exit(1); }
            if (tam > 0) fwrite(buf + pos, 1, tam, f);
            fclose(f);
            pos += tam;
        }
    }
}
 
/* ==================== Árbol de Huffman ==================== */
 
typedef struct Nodo {
    int byte;            /* -1 si es nodo interno */
    uint64_t freq;
    struct Nodo *izq, *der;
} Nodo;
 
static Nodo *nodo_nuevo(int byte, uint64_t freq, Nodo *izq, Nodo *der) {
    Nodo *n = malloc(sizeof(Nodo));
    n->byte = byte; n->freq = freq; n->izq = izq; n->der = der;
    return n;
}
 
/* Min-heap sencillo basado en arreglo */
typedef struct {
    Nodo **datos;
    int tam;
} Heap;
 
static void heap_subir(Heap *h, int i) {
    while (i > 0) {
        int padre = (i - 1) / 2;
        if (h->datos[padre]->freq <= h->datos[i]->freq) break;
        Nodo *tmp = h->datos[padre]; h->datos[padre] = h->datos[i]; h->datos[i] = tmp;
        i = padre;
    }
}
 
static void heap_bajar(Heap *h, int i) {
    while (1) {
        int izq = 2 * i + 1, der = 2 * i + 2, menor = i;
        if (izq < h->tam && h->datos[izq]->freq < h->datos[menor]->freq) menor = izq;
        if (der < h->tam && h->datos[der]->freq < h->datos[menor]->freq) menor = der;
        if (menor == i) break;
        Nodo *tmp = h->datos[menor]; h->datos[menor] = h->datos[i]; h->datos[i] = tmp;
        i = menor;
    }
}
 
static void heap_push(Heap *h, Nodo *n) {
    h->datos[h->tam++] = n;
    heap_subir(h, h->tam - 1);
}
 
static Nodo *heap_pop(Heap *h) {
    Nodo *top = h->datos[0];
    h->datos[0] = h->datos[--h->tam];
    heap_bajar(h, 0);
    return top;
}
 
static Nodo *construir_arbol(uint64_t frecuencias[256]) {
    Heap h;
    h.datos = malloc(sizeof(Nodo *) * 256);
    h.tam = 0;
 
    for (int i = 0; i < 256; i++)
        if (frecuencias[i] > 0)
            heap_push(&h, nodo_nuevo(i, frecuencias[i], NULL, NULL));
 
    if (h.tam == 0) { free(h.datos); return NULL; }
 
    if (h.tam == 1) {
        Nodo *unico = heap_pop(&h);
        free(h.datos);
        return nodo_nuevo(-1, unico->freq, unico, NULL);
    }
 
    while (h.tam > 1) {
        Nodo *a = heap_pop(&h);
        Nodo *b = heap_pop(&h);
        heap_push(&h, nodo_nuevo(-1, a->freq + b->freq, a, b));
    }
    Nodo *raiz = heap_pop(&h);
    free(h.datos);
    return raiz;
}
 
static void liberar_arbol(Nodo *n) {
    if (!n) return;
    liberar_arbol(n->izq);
    liberar_arbol(n->der);
    free(n);
}
 
#define MAX_BITS 64
 
typedef struct {
    uint8_t bits[MAX_BITS];
    int len;
} Codigo;
 
static void generar_codigos(Nodo *n, uint8_t *pila, int profundidad, Codigo codigos[256]) {
    if (!n) return;
    if (n->byte != -1) {
        if (profundidad == 0) {
            /* árbol con un solo símbolo distinto */
            codigos[n->byte].len = 1;
            codigos[n->byte].bits[0] = 0;
        } else {
            codigos[n->byte].len = profundidad;
            memcpy(codigos[n->byte].bits, pila, profundidad);
        }
        return;
    }
    pila[profundidad] = 0;
    generar_codigos(n->izq, pila, profundidad + 1, codigos);
    pila[profundidad] = 1;
    generar_codigos(n->der, pila, profundidad + 1, codigos);
}
 
/* ==================== Empaquetado de bits ==================== */
 
typedef struct {
    uint8_t *buf;
    size_t capacidad;
    size_t byte_pos;
    int bit_pos; /* 0..7 */
} EscritorBits;
 
static void eb_init(EscritorBits *e) {
    e->capacidad = 4096;
    e->buf = malloc(e->capacidad);
    memset(e->buf, 0, e->capacidad);
    e->byte_pos = 0;
    e->bit_pos = 0;
}
 
static void eb_escribir_bit(EscritorBits *e, uint8_t bit) {
    if (e->byte_pos >= e->capacidad) {
        e->capacidad *= 2;
        e->buf = realloc(e->buf, e->capacidad);
        memset(e->buf + e->capacidad / 2, 0, e->capacidad / 2);
    }
    if (bit) e->buf[e->byte_pos] |= (1 << (7 - e->bit_pos));
    e->bit_pos++;
    if (e->bit_pos == 8) { e->bit_pos = 0; e->byte_pos++; }
}
 
typedef struct {
    const uint8_t *buf;
    size_t total_bytes;
    size_t byte_pos;
    int bit_pos;
} LectorBits;
 
static uint8_t lb_leer_bit(LectorBits *l) {
    uint8_t bit = (l->buf[l->byte_pos] >> (7 - l->bit_pos)) & 1;
    l->bit_pos++;
    if (l->bit_pos == 8) { l->bit_pos = 0; l->byte_pos++; }
    return bit;
}
 
/* ==================== Formato del archivo .huff ==================== */
/* [magic "HUF1"][u64 tam_original][u8 padding][256 x u64 frecuencias][datos comprimidos] */
 
static const char MAGIC[4] = {'H', 'U', 'F', '1'};
 
static void comprimir(const char *origen, const char *destino) {
    ListaEntradas lista;
    lista_init(&lista);
 
    if (es_directorio(origen)) {
        recorrer(origen, "", &lista);
    } else {
        Entrada *e = lista_agregar(&lista);
        e->tipo = 0;
        e->ruta = strdup(nombre_base(origen));
        e->datos = leer_archivo(origen, &e->tam);
    }
 
    uint64_t tam_original;
    uint8_t *contenedor = serializar(&lista, &tam_original);
 
    /* Frecuencias de bytes en el contenedor */
    uint64_t frecuencias[256] = {0};
    for (uint64_t i = 0; i < tam_original; i++) frecuencias[contenedor[i]]++;
 
    Nodo *arbol = construir_arbol(frecuencias);
    Codigo codigos[256] = {0};
    uint8_t pila[MAX_BITS];
    if (arbol) generar_codigos(arbol, pila, 0, codigos);
 
    EscritorBits eb;
    eb_init(&eb);
    for (uint64_t i = 0; i < tam_original; i++) {
        Codigo *c = &codigos[contenedor[i]];
        for (int b = 0; b < c->len; b++)
            eb_escribir_bit(&eb, c->bits[b]);
    }
    uint8_t padding = eb.bit_pos ? (uint8_t)(8 - eb.bit_pos) : 0;
    size_t tam_comprimido = eb.byte_pos + (eb.bit_pos ? 1 : 0);
 
    FILE *f = fopen(destino, "wb");
    if (!f) { perror(destino); exit(1); }
    fwrite(MAGIC, 1, 4, f);
    fwrite(&tam_original, sizeof(uint64_t), 1, f);
    fwrite(&padding, sizeof(uint8_t), 1, f);
    fwrite(frecuencias, sizeof(uint64_t), 256, f);
    fwrite(eb.buf, 1, tam_comprimido, f);
    fclose(f);
 
    uint64_t tam_cabecera = 4 + 8 + 1 + 256 * 8;
    uint64_t tam_final = tam_cabecera + tam_comprimido;
 
    printf("Archivo/carpeta comprimido correctamente: %s\n", destino);
    printf("Elementos incluidos: %zu\n", lista.cuenta);
    printf("Tamaño original (sin comprimir): %llu bytes\n", (unsigned long long)tam_original);
    printf("Tamaño final: %llu bytes\n", (unsigned long long)tam_final);
    if (tam_original > 0) {
        double ratio = (1.0 - (double)tam_final / (double)tam_original) * 100.0;
        printf("Reducción: %.2f%%\n", ratio);
    }
 
    liberar_arbol(arbol);
    free(contenedor);
    free(eb.buf);
    for (size_t i = 0; i < lista.cuenta; i++) {
        free(lista.items[i].ruta);
        free(lista.items[i].datos);
    }
    free(lista.items);
}
 
static void descomprimir(const char *origen, const char *destino) {
    FILE *f = fopen(origen, "rb");
    if (!f) { perror(origen); exit(1); }
 
    char magic[4];
    fread(magic, 1, 4, f);
    if (memcmp(magic, MAGIC, 4) != 0) {
        fprintf(stderr, "Archivo no reconocido (formato incorrecto)\n");
        exit(1);
    }
 
    uint64_t tam_original;
    uint8_t padding;
    fread(&tam_original, sizeof(uint64_t), 1, f);
    fread(&padding, sizeof(uint8_t), 1, f);
 
    uint64_t frecuencias[256];
    fread(frecuencias, sizeof(uint64_t), 256, f);
 
    /* Leer el resto del archivo (datos comprimidos) */
    long pos_actual = ftell(f);
    fseek(f, 0, SEEK_END);
    long fin = ftell(f);
    fseek(f, pos_actual, SEEK_SET);
    size_t tam_comprimido = fin - pos_actual;
 
    uint8_t *datos_comprimidos = malloc(tam_comprimido);
    fread(datos_comprimidos, 1, tam_comprimido, f);
    fclose(f);
 
    Nodo *arbol = construir_arbol(frecuencias);
 
    uint8_t *contenedor = malloc(tam_original > 0 ? tam_original : 1);
    LectorBits lb = { datos_comprimidos, tam_comprimido, 0, 0 };
 
    uint64_t producidos = 0;
    while (producidos < tam_original) {
        Nodo *nodo = arbol;
        while (nodo->byte == -1) {
            uint8_t bit = lb_leer_bit(&lb);
            nodo = bit ? nodo->der : nodo->izq;
        }
        contenedor[producidos++] = (uint8_t)nodo->byte;
    }
 
    extraer(contenedor, tam_original, destino);
 
    printf("Descompresión completada en: %s/\n", destino);
 
    liberar_arbol(arbol);
    free(contenedor);
    free(datos_comprimidos);
}
 
/* ==================== main ==================== */
 
static void uso(const char *prog) {
    fprintf(stderr,
        "Uso:\n"
        "  Comprimir archivo o carpeta:  %s -c origen destino.huff\n"
        "  Descomprimir:                 %s -d origen.huff destino/\n",
        prog, prog);
}
 
int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1], "-c") != 0 && strcmp(argv[1], "-d") != 0)) {
        uso(argv[0]);
        return 1;
    }
 
    if (strcmp(argv[1], "-c") == 0)
        comprimir(argv[2], argv[3]);
    else
        descomprimir(argv[2], argv[3]);
 
    return 0;
}
