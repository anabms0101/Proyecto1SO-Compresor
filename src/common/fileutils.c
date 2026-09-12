#include "fileutils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

int read_entire_file(const char *path, unsigned char **out_buf, uint64_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    rewind(f);

    unsigned char *buf = malloc((size_t)size > 0 ? (size_t)size : 1);
    if (!buf) { fclose(f); return -1; }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if ((long)read != size) { free(buf); return -1; }

    *out_buf = buf;
    *out_len = (uint64_t)size;
    return 0;
}

int write_entire_file(const char *path, const unsigned char *buf, uint64_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len > 0) {
        size_t written = fwrite(buf, 1, (size_t)len, f);
        if (written != (size_t)len) { fclose(f); return -1; }
    }
    fclose(f);
    return 0;
}

int make_dirs_recursive(const char *path) {
    char tmp[4096];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;

    strcpy(tmp, path);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

void join_path(char *dst, size_t dst_size, const char *dir, const char *name) {
    snprintf(dst, dst_size, "%s/%s", dir, name);
}

static void list_recurse(const char *base_dir, const char *rel_prefix,
                          int recursive, FileList *out, int *capacity) {
    char full_dir[4096];
    if (rel_prefix[0] == '\0') {
        snprintf(full_dir, sizeof(full_dir), "%s", base_dir);
    } else {
        snprintf(full_dir, sizeof(full_dir), "%s/%s", base_dir, rel_prefix);
    }

    DIR *d = opendir(full_dir);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char rel_path[4096];
        if (rel_prefix[0] == '\0') {
            snprintf(rel_path, sizeof(rel_path), "%s", entry->d_name);
        } else {
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_prefix, entry->d_name);
        }

        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, rel_path);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (recursive) {
                list_recurse(base_dir, rel_path, recursive, out, capacity);
            }
        } else if (S_ISREG(st.st_mode)) {
            if (out->count == *capacity) {
                *capacity = (*capacity == 0) ? 16 : (*capacity * 2);
                out->paths = realloc(out->paths, sizeof(char *) * (size_t)(*capacity));
            }
            out->paths[out->count] = strdup(rel_path);
            out->count++;
        }
    }
    closedir(d);
}

int list_directory_files(const char *dir_path, int recursive, FileList *out) {
    out->paths = NULL;
    out->count = 0;
    int capacity = 0;
    list_recurse(dir_path, "", recursive, out, &capacity);
    return 0;
}

void file_list_free(FileList *list) {
    for (int i = 0; i < list->count; i++) free(list->paths[i]);
    free(list->paths);
    list->paths = NULL;
    list->count = 0;
}

uint64_t directory_total_size(const char *dir_path, int recursive) {
    FileList files;
    list_directory_files(dir_path, recursive, &files);

    uint64_t total = 0;
    for (int i = 0; i < files.count; i++) {
        char full_path[4096];
        join_path(full_path, sizeof(full_path), dir_path, files.paths[i]);
        struct stat st;
        if (stat(full_path, &st) == 0) total += (uint64_t)st.st_size;
    }

    file_list_free(&files);
    return total;
}

uint64_t file_size_bytes(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_size;
}
