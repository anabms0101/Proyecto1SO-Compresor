#include "huffman.h"
#include <stdlib.h>
#include <string.h>

typedef struct HNode {
    uint64_t freq;
    int symbol;          /* 0-255 si es hoja, -1 si es interno */
    struct HNode *left;
    struct HNode *right;
} HNode;

static HNode *hnode_new(uint64_t freq, int symbol, HNode *l, HNode *r) {
    HNode *n = malloc(sizeof(HNode));
    n->freq = freq;
    n->symbol = symbol;
    n->left = l;
    n->right = r;
    return n;
}

static void hnode_free(HNode *n) {
    if (!n) return;
    hnode_free(n->left);
    hnode_free(n->right);
    free(n);
}

/* Cola de prioridad muy simple (arreglo + busqueda lineal del minimo).
 * Con a lo sumo 256 simbolos, un heap real es innecesario: O(n^2) aqui
 * es trivial en tiempo de ejecucion. */
typedef struct {
    HNode **items;
    int count;
    int capacity;
} PQueue;

static void pq_init(PQueue *q, int capacity) {
    q->items = malloc(sizeof(HNode *) * (size_t)capacity);
    q->count = 0;
    q->capacity = capacity;
}

static void pq_push(PQueue *q, HNode *n) {
    q->items[q->count++] = n;
}

static HNode *pq_pop_min(PQueue *q) {
    int min_idx = 0;
    for (int i = 1; i < q->count; i++) {
        if (q->items[i]->freq < q->items[min_idx]->freq) min_idx = i;
    }
    HNode *n = q->items[min_idx];
    q->items[min_idx] = q->items[q->count - 1];
    q->count--;
    return n;
}

/* Construye el arbol de Huffman a partir de la tabla de frecuencias.
 * Devuelve NULL si no hay ningun simbolo (buffer vacio). */
static HNode *build_tree(const uint64_t freq[256]) {
    PQueue q;
    pq_init(&q, 256);

    for (int i = 0; i < 256; i++) {
        if (freq[i] > 0) {
            pq_push(&q, hnode_new(freq[i], i, NULL, NULL));
        }
    }

    if (q.count == 0) {
        free(q.items);
        return NULL;
    }

    /* Caso especial: un solo simbolo distinto. Se crea un nodo interno
     * artificial para que el simbolo tenga codigo de longitud 1 (no 0). */
    if (q.count == 1) {
        HNode *only = q.items[0];
        HNode *root = hnode_new(only->freq, -1, only, NULL);
        free(q.items);
        return root;
    }

    while (q.count > 1) {
        HNode *a = pq_pop_min(&q);
        HNode *b = pq_pop_min(&q);
        HNode *parent = hnode_new(a->freq + b->freq, -1, a, b);
        pq_push(&q, parent);
    }

    HNode *root = q.items[0];
    free(q.items);
    return root;
}

typedef struct {
    unsigned char bits[256]; /* 0/1 por posicion, hasta 256 bits de profundidad (mas que suficiente) */
    int len;
} Code;

static void build_codes(HNode *node, Code *codes, unsigned char *prefix, int depth) {
    if (!node) return;

    if (node->symbol >= 0) {
        Code *c = &codes[node->symbol];
        c->len = depth > 0 ? depth : 1;
        if (depth == 0) {
            c->bits[0] = 0; /* unico simbolo: codigo "0" */
        } else {
            memcpy(c->bits, prefix, (size_t)depth);
        }
        return;
    }

    if (node->left) {
        prefix[depth] = 0;
        build_codes(node->left, codes, prefix, depth + 1);
    }
    if (node->right) {
        prefix[depth] = 1;
        build_codes(node->right, codes, prefix, depth + 1);
    }
}

/* --- Empaquetado de bits en bytes (MSB primero) --- */

typedef struct {
    unsigned char *buf;
    uint64_t capacity;
    uint64_t bit_pos; /* posicion actual en bits */
} BitWriter;

static void bw_init(BitWriter *bw, uint64_t initial_capacity_bytes) {
    bw->buf = calloc(initial_capacity_bytes > 0 ? initial_capacity_bytes : 1, 1);
    bw->capacity = initial_capacity_bytes;
    bw->bit_pos = 0;
}

static void bw_ensure(BitWriter *bw, uint64_t extra_bits) {
    uint64_t needed_bytes = (bw->bit_pos + extra_bits + 7) / 8;
    if (needed_bytes > bw->capacity) {
        uint64_t new_cap = bw->capacity == 0 ? 64 : bw->capacity;
        while (new_cap < needed_bytes) new_cap *= 2;
        bw->buf = realloc(bw->buf, new_cap);
        memset(bw->buf + bw->capacity, 0, new_cap - bw->capacity);
        bw->capacity = new_cap;
    }
}

static void bw_put_bit(BitWriter *bw, unsigned char bit) {
    bw_ensure(bw, 1);
    uint64_t byte_idx = bw->bit_pos / 8;
    int bit_idx = 7 - (int)(bw->bit_pos % 8); /* MSB primero */
    if (bit) {
        bw->buf[byte_idx] |= (unsigned char)(1u << bit_idx);
    }
    bw->bit_pos++;
}

static void bw_put_bits(BitWriter *bw, const unsigned char *bits, int len) {
    bw_ensure(bw, (uint64_t)len);
    for (int i = 0; i < len; i++) {
        bw_put_bit(bw, bits[i]);
    }
}

typedef struct {
    const unsigned char *buf;
    uint64_t bit_pos;
    uint64_t total_bits;
} BitReader;

static void br_init(BitReader *br, const unsigned char *buf, uint64_t total_bits) {
    br->buf = buf;
    br->bit_pos = 0;
    br->total_bits = total_bits;
}

static int br_get_bit(BitReader *br) {
    if (br->bit_pos >= br->total_bits) return -1;
    uint64_t byte_idx = br->bit_pos / 8;
    int bit_idx = 7 - (int)(br->bit_pos % 8);
    int bit = (br->buf[byte_idx] >> bit_idx) & 1;
    br->bit_pos++;
    return bit;
}

int huffman_compress(const unsigned char *input, uint64_t in_len, HuffmanResult *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    for (uint64_t i = 0; i < in_len; i++) {
        out->freq[input[i]]++;
    }

    if (in_len == 0) {
        out->data = NULL;
        out->data_len = 0;
        out->bit_count = 0;
        return 0;
    }

    HNode *root = build_tree(out->freq);
    if (!root) return -1;

    Code codes[256];
    memset(codes, 0, sizeof(codes));
    unsigned char prefix[256];
    build_codes(root, codes, prefix, 0);

    BitWriter bw;
    bw_init(&bw, (in_len / 4) + 16); /* estimacion inicial */

    for (uint64_t i = 0; i < in_len; i++) {
        Code *c = &codes[input[i]];
        bw_put_bits(&bw, c->bits, c->len);
    }

    out->data = bw.buf;
    out->bit_count = bw.bit_pos;
    out->data_len = (bw.bit_pos + 7) / 8;

    hnode_free(root);
    return 0;
}

int huffman_decompress(const uint64_t freq[256],
                        const unsigned char *data,
                        uint64_t bit_count,
                        unsigned char *output,
                        uint64_t original_len) {
    if (original_len == 0) return 0;
    if (!data || !output) return -1;

    HNode *root = build_tree(freq);
    if (!root) return -1;

    BitReader br;
    br_init(&br, data, bit_count);

    uint64_t written = 0;
    HNode *node = root;

    /* Caso especial: un unico simbolo distinto en todo el archivo. */
    if (root->symbol < 0 && root->left && !root->right && root->left->symbol >= 0) {
        unsigned char sym = (unsigned char)root->left->symbol;
        for (uint64_t i = 0; i < original_len; i++) output[i] = sym;
        hnode_free(root);
        return 0;
    }

    while (written < original_len) {
        int bit = br_get_bit(&br);
        if (bit < 0) { hnode_free(root); return -1; }

        node = bit ? node->right : node->left;
        if (!node) { hnode_free(root); return -1; }

        if (node->symbol >= 0) {
            output[written++] = (unsigned char)node->symbol;
            node = root;
        }
    }

    hnode_free(root);
    return 0;
}

void huffman_result_free(HuffmanResult *r) {
    if (!r) return;
    free(r->data);
    r->data = NULL;
    r->data_len = 0;
    r->bit_count = 0;
}
