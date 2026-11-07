#pragma once

// Wear-levelled FAT cache in the ESP32-S3's onboard flash at /storage.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define STORAGE_ROOT "/storage/abs"

esp_err_t storage_init(void);
bool storage_ready(void);
// Free and total space in bytes (0 if flash storage is unavailable).
void storage_space(uint64_t *free_bytes, uint64_t *total_bytes);

// Whole-file helpers. read returns a NUL-terminated PSRAM buffer the caller frees.
char *storage_read_file(const char *path, size_t *len);
// Writes via a temporary file and rename, so a crash never leaves a half-written file.
esp_err_t storage_write_file(const char *path, const void *data, size_t len);
// Creates the directory and any missing parents.
esp_err_t storage_mkdirs(const char *path);
// Deletes a directory tree.
esp_err_t storage_remove_tree(const char *path);
