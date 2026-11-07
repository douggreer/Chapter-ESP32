#pragma once

#include <stddef.h>

#define DEVICE_HISTORY_LIMIT 12

// Device-only playback history, retained through ESP32 deep sleep and reset by a full power-on.

typedef struct {
    char id[40];
    char title[128];
    char author[96];
} device_history_entry_t;

void device_history_add(const char *id, const char *title, const char *author);
int device_history_copy(device_history_entry_t *out, size_t capacity);
