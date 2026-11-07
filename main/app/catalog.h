#pragma once

// Which library is shown, and loading it (from the onboard flash cache or the server).
//
// The selected library id is kept in NVS. Each library has its own cache directory,
// STORAGE_ROOT/lib/<library id>/ (items.json, covers/, episodes/), so switching libraries never
// overwrites another library's cache. The user record (progress for everything) and the list of
// libraries are shared: STORAGE_ROOT/me.json and libraries.json.
//
// Loading functions block on the network; call them from the main task, not the UI.

#include <stdbool.h>
#include <stddef.h>
#include "abs_api.h"

void catalog_init(void);

// Currently selected library ("" until known) and its cache directory ("" if flash storage is unavailable).
const char *catalog_library_id(void);
bool catalog_library_is_podcast(void);
void catalog_dir(char *out, size_t len);

// Shows the selected library from the onboard flash cache. Returns false if there is none.
bool catalog_load_cached(void);
// Fetches the library list and the selected library's items from the server and shows them.
bool catalog_load_network(bool quiet);
// Makes another library the selected one (saved); the caller then loads it.
void catalog_select(const char *library_id);
typedef struct {
    char id[40];
    char title[128];
    char sort_title[128];
    char author[96];
    char authors[6][64];
    uint8_t author_count;
    char series[128];
    char series_sequence[24];
    int series_total;
    double duration;
    double current_time;
    double last_update;
    float progress;
    bool finished;
    bool podcast;
} catalog_web_item_t;

// Thread-safe snapshots and playback requests used by the local web controls.
int catalog_copy_web_items(catalog_web_item_t *out, size_t capacity);
bool catalog_play_book(const char *item_id);
