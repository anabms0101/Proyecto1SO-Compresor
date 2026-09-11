CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Iinclude
LDFLAGS =

BUILD_DIR = build
BIN_DIR = bin

COMMON_SRC = src/common/md5.c src/common/huffman.c src/common/archive.c src/common/fileutils.c
COMMON_OBJ = $(COMMON_SRC:src/common/%.c=$(BUILD_DIR)/common_%.o)

.PHONY: all serial fork thread gui clean

all: serial

# --- Version serial (Fase 1) ---
serial: $(BIN_DIR)/compresor_serial $(BIN_DIR)/descompresor_serial

$(BIN_DIR)/compresor_serial: src/serial/compressor_serial.c $(COMMON_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/descompresor_serial: src/serial/decompressor_serial.c $(COMMON_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- Version paralela con fork() + IPC (Fase 2) ---
fork: $(BIN_DIR)/compressor_parallel $(BIN_DIR)/decompressor_parallel

$(BIN_DIR)/compressor_parallel: src/fork/compressor_parallel.c $(COMMON_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/decompressor_parallel: src/fork/decompressor_parallel.c $(COMMON_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- Version concurrente con pthreads + memoria compartida (Fase 3, pendiente) ---
thread:
	@echo "TODO: implementar compresor_thread / descompresor_thread (usa -pthread)"

# --- Interfaz grafica GTK (Fase 4, pendiente) ---
gui:
	@echo "TODO: implementar GUI con GTK (usa pkg-config --cflags --libs gtk4)"

$(BUILD_DIR)/common_%.o: src/common/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
