CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Iinclude
LDFLAGS =
PTHREAD_FLAGS = -pthread

BUILD_DIR = build
BIN_DIR = bin

COMMON_SRC = src/common/md5.c src/common/huffman.c src/common/archive.c src/common/fileutils.c src/common/binio.c src/common/entry_codec.c
COMMON_OBJ = $(COMMON_SRC:src/common/%.c=$(BUILD_DIR)/common_%.o)

.PHONY: all serial fork thread gui clean

all: serial fork thread gui

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

# --- Version concurrente con pthreads + memoria compartida (Fase 3) ---
thread: $(BIN_DIR)/compressor_thread $(BIN_DIR)/decompressor_thread

$(BIN_DIR)/compressor_thread: src/thread/compressor_thread.c $(COMMON_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(PTHREAD_FLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/decompressor_thread: src/thread/decompressor_thread.c $(COMMON_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(PTHREAD_FLAGS) -o $@ $^ $(LDFLAGS)

# --- Interfaz grafica GTK4 (Fase 4) ---
# Requiere los headers de desarrollo de GTK4: sudo apt install libgtk-4-dev
GUI_CFLAGS = $(shell pkg-config --cflags gtk4)
GUI_LIBS = $(shell pkg-config --libs gtk4)

gui: $(BIN_DIR)/gui_comparador

# La GUI NO se linkea contra los modulos comunes (md5/huffman/archive):
# invoca los 6 binarios ya compilados como subprocesos (ver comentario al
# inicio de main_gui.c) y solo necesita GTK4 + GLib.
$(BIN_DIR)/gui_comparador: src/gui/main_gui.c | $(BIN_DIR) serial fork thread
	$(CC) $(CFLAGS) $(GUI_CFLAGS) -o $@ $< $(GUI_LIBS)

$(BUILD_DIR)/common_%.o: src/common/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
