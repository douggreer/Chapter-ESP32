#include "catalog.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "download.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "player.h"
#include "power.h"
#include "storage.h"
#include "ui.h"

static const char *TAG = "catalog";

#define LIBRARIES_JSON STORAGE_ROOT "/libraries.json"
#define ME_JSON        STORAGE_ROOT "/me.json"

static void clean_item_cache_for_boot(const char *dir);

static void pump_catalog_ui(void)
{
    ui_process();
    // A manual refresh requested during this load is already being fulfilled.
    (void)ui_take_refresh_request();
    if (ui_take_shutdown_request()) power_shutdown_now();
}

static char s_library[40];
static abs_library_t *s_libs;
static int s_lib_count;
static abs_book_t *s_books;
static int s_count;
static SemaphoreHandle_t s_books_lock;
static SemaphoreHandle_t s_catalog_lock;
static volatile bool s_account_switch_pending;

/* ---------- selection ---------- */

static bool save_selection(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("abs", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "library", s_library);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "could not save selected library: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

const char *catalog_library_id(void)
{
    return s_library;
}

static const abs_library_t *current(void)
{
    for (int i = 0; i < s_lib_count; i++) {
        if (strcmp(s_libs[i].id, s_library) == 0) return &s_libs[i];
    }
    return NULL;
}

bool catalog_library_is_podcast(void)
{
    const abs_library_t *l = current();
    return l && l->podcast;
}

void catalog_dir(char *out, size_t len)
{
    if (!storage_ready() || !s_library[0]) {
        out[0] = 0;
        return;
    }
    snprintf(out, len, STORAGE_ROOT "/lib/%s", s_library);
}

// Picks a library if none is selected (or the selected one is gone): the first book library.
static void ensure_selection(void)
{
    if (current()) return;
    for (int i = 0; i < s_lib_count; i++) {
        if (!s_libs[i].podcast || i == s_lib_count - 1) {
            strlcpy(s_library, s_libs[i].id, sizeof(s_library));
            (void)save_selection();
            return;
        }
    }
}

static void set_libraries(abs_library_t *libs, int n)
{
    free(s_libs);
    s_libs = libs;
    s_lib_count = n;
    ESP_LOGI(TAG, "%d librar%s on the server", n, n == 1 ? "y" : "ies");
    ensure_selection();
    const abs_library_t *l = current();
    abs_set_library_name(l ? l->name : "");
    ui_set_libraries(s_libs, s_lib_count, s_library);
}

void catalog_select(const char *library_id)
{
    if (!s_catalog_lock || s_account_switch_pending) return;
    xSemaphoreTake(s_catalog_lock, portMAX_DELAY);
    if (s_account_switch_pending) {
        xSemaphoreGive(s_catalog_lock);
        return;
    }
    strlcpy(s_library, library_id, sizeof(s_library));
    (void)save_selection();
    const abs_library_t *l = current();
    abs_set_library_name(l ? l->name : "");
    ui_set_libraries(s_libs, s_lib_count, s_library);
    ESP_LOGI(TAG, "selected library %s", l ? l->name : library_id);
    xSemaphoreGive(s_catalog_lock);
}

int catalog_copy_web_items(size_t offset, catalog_web_item_t *out, size_t capacity)
{
    if (!out || capacity == 0 || !s_books_lock || s_account_switch_pending) return 0;
    xSemaphoreTake(s_books_lock, portMAX_DELAY);
    if (s_account_switch_pending) {
        xSemaphoreGive(s_books_lock);
        return 0;
    }
    size_t seen = 0;
    int count = 0;
    for (int i = 0; i < s_count && count < (int)capacity; ++i) {
        const abs_book_t *src = &s_books[i];
        if (src->podcast) continue;
        if (seen++ < offset) continue;
        catalog_web_item_t *dst = &out[count++];
        strlcpy(dst->id, src->id, sizeof(dst->id));
        strlcpy(dst->title, src->title ? src->title : "Untitled", sizeof(dst->title));
        strlcpy(dst->sort_title, src->sort_title ? src->sort_title : dst->title, sizeof(dst->sort_title));
        strlcpy(dst->author, src->author ? src->author : "", sizeof(dst->author));
        dst->author_count = src->author_count;
        for (int a = 0; a < src->author_count; ++a) {
            strlcpy(dst->authors[a], src->authors[a] ? src->authors[a] : "", sizeof(dst->authors[a]));
        }
        strlcpy(dst->series, src->series_name, sizeof(dst->series));
        strlcpy(dst->series_sequence, src->series_sequence, sizeof(dst->series_sequence));
        dst->series_total = src->series_total;
        dst->duration = src->duration;
        dst->current_time = src->current_time;
        dst->last_update = src->last_update;
        dst->progress = src->progress;
        dst->finished = src->finished;
        dst->podcast = src->podcast;
    }
    xSemaphoreGive(s_books_lock);
    return count;
}

bool catalog_play_book(const char *item_id)
{
    if (!item_id || !item_id[0] || !s_books_lock || s_account_switch_pending) return false;
    bool found = false;
    xSemaphoreTake(s_books_lock, portMAX_DELAY);
    if (s_account_switch_pending) {
        xSemaphoreGive(s_books_lock);
        return false;
    }
    for (int i = 0; i < s_count; ++i) {
        if (strcmp(s_books[i].id, item_id) != 0 || s_books[i].podcast) continue;
        player_open(&s_books[i]);
        found = true;
        break;
    }
    xSemaphoreGive(s_books_lock);
    return found;
}

/* ---------- cache ---------- */

// Before per-library caches, items.json and covers/ sat directly in STORAGE_ROOT. Move them into
// the directory of the library they came from (named in the items themselves).
static void migrate(void)
{
    const char *old_items = STORAGE_ROOT "/items.json";
    struct stat st;
    if (stat(old_items, &st) != 0) return;
    char *json = storage_read_file(old_items, NULL);
    const char *p = json ? strstr(json, "\"libraryId\":\"") : NULL;
    char id[40] = "";
    if (p) sscanf(p + 13, "%39[^\"]", id);
    free(json);
    if (id[0]) {
        char dir[128], dst[160];
        snprintf(dir, sizeof(dir), STORAGE_ROOT "/lib/%s", id);
        storage_mkdirs(dir);
        snprintf(dst, sizeof(dst), "%s/items.json", dir);
        rename(old_items, dst);
        snprintf(dst, sizeof(dst), "%s/covers", dir);
        rename(STORAGE_ROOT "/covers", dst);
        if (!s_library[0]) {
            strlcpy(s_library, id, sizeof(s_library));
            (void)save_selection();
        }
        ESP_LOGI(TAG, "moved the old cache into library %.8s", id);
    } else {
        unlink(old_items);
    }
    unlink(STORAGE_ROOT "/library.txt");
}

void catalog_init(void)
{
    if (!s_books_lock) s_books_lock = xSemaphoreCreateMutex();
    if (!s_catalog_lock) s_catalog_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    size_t len = sizeof(s_library);
    if (nvs_open("abs", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "library", s_library, &len) != ESP_OK) s_library[0] = 0;
        nvs_close(h);
    }
    if (!storage_ready()) return;
    migrate();
    char *json = storage_read_file(LIBRARIES_JSON, NULL);
    abs_library_t *libs = NULL;
    int n = 0;
    if (json && abs_parse_libraries(json, &libs, &n) == ESP_OK && n > 0) set_libraries(libs, n);
    else free(libs);
    free(json);
    char dir[96];
    catalog_dir(dir, sizeof(dir));
    if (dir[0]) clean_item_cache_for_boot(dir);
}

static bool unlink_if_exists(const char *path)
{
    if (unlink(path) == 0 || errno == ENOENT) return true;
    ESP_LOGE(TAG, "could not remove account cache %s (errno %d)", path, errno);
    return false;
}

static bool remove_tree_if_exists(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT) return true;
        ESP_LOGE(TAG, "could not inspect account cache %s (errno %d)", path, errno);
        return false;
    }
    if (storage_remove_tree(path) == ESP_OK) return true;
    ESP_LOGE(TAG, "could not remove account cache directory %s", path);
    return false;
}

bool catalog_prepare_account_switch(void)
{
    if (!s_catalog_lock) return false;
    xSemaphoreTake(s_catalog_lock, portMAX_DELAY);
    s_account_switch_pending = true;

    // Clear account-specific progress plus per-library metadata. Offline audio lives in /dl and
    // is deliberately retained.
    s_library[0] = 0;
    bool ok = save_selection();
    if (storage_ready()) {
        ok &= unlink_if_exists(ME_JSON);
        ok &= unlink_if_exists(LIBRARIES_JSON);
        ok &= unlink_if_exists(STORAGE_ROOT "/items.json");
        ok &= unlink_if_exists(STORAGE_ROOT "/library.txt");
        ok &= remove_tree_if_exists(STORAGE_ROOT "/lib");
        ok &= remove_tree_if_exists(STORAGE_ROOT "/covers");
    }
    if (ok) ESP_LOGI(TAG, "cleared cached library and progress for Audiobookshelf account change");
    xSemaphoreGive(s_catalog_lock);
    return ok;
}

void catalog_cancel_account_switch(void)
{
    s_account_switch_pending = false;
}

static void show(abs_book_t *fresh, int n, bool cached)
{
    if (!s_books_lock) s_books_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_books_lock, portMAX_DELAY);
    abs_book_t *old = s_books;
    const int old_count = s_count;
    s_books = fresh;
    s_count = n;
    ui_set_books(fresh, n);
    ui_set_source(cached);
    abs_free_books(old, old_count);
    xSemaphoreGive(s_books_lock);
}

typedef struct {
    char dir[96];
    uint32_t generation;
} book_cache_writer_t;

static void remove_old_item_pages(const char *dir, uint32_t keep_generation, bool keep_generation_files)
{
    DIR *directory = opendir(dir);
    if (!directory) return;
    char keep_prefix[32];
    snprintf(keep_prefix, sizeof(keep_prefix), "items-%08lx-", (unsigned long)keep_generation);
    struct dirent *entry;
    char path[160];
    while ((entry = readdir(directory))) {
        if (strncmp(entry->d_name, "items-", 6) != 0 || !strstr(entry->d_name, ".json")) continue;
        if (keep_generation_files && strncmp(entry->d_name, keep_prefix, strlen(keep_prefix)) == 0) continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) < (int)sizeof(path)) unlink(path);
    }
    closedir(directory);
}

static bool finish_book_cache(book_cache_writer_t *writer, const abs_book_t *books, int count)
{
    if (!storage_ready()) return false;
    const int64_t started = esp_timer_get_time();
    const int pages = (count + ABS_BOOK_PAGE_SIZE - 1) / ABS_BOOK_PAGE_SIZE;
    for (int page = 0; page < pages; ++page) {
        char path[160];
        snprintf(path, sizeof(path), "%s/items-%08lx-%03d.json", writer->dir,
                 (unsigned long)writer->generation, page);
        const int offset = page * ABS_BOOK_PAGE_SIZE;
        int page_count = count - offset;
        if (page_count > ABS_BOOK_PAGE_SIZE) page_count = ABS_BOOK_PAGE_SIZE;
        char *json = abs_books_cache_json(books + offset, page_count);
        if (!json || storage_write_file(path, json, strlen(json)) != ESP_OK) {
            free(json);
            ESP_LOGW(TAG, "could not write compact library cache page %d", page);
            return false;
        }
        free(json);
        pump_catalog_ui();
    }
    char index_path[128], manifest[96];
    snprintf(index_path, sizeof(index_path), "%s/items-index.txt", writer->dir);
    int len = snprintf(manifest, sizeof(manifest), "2 %08lx %d %d\n",
                       (unsigned long)writer->generation, pages, count);
    if (storage_write_file(index_path, manifest, len) != ESP_OK) return false;
    char legacy_path[128];
    snprintf(legacy_path, sizeof(legacy_path), "%s/items.json", writer->dir);
    unlink(legacy_path);
    remove_old_item_pages(writer->dir, writer->generation, true);
    ESP_LOGI(TAG, "cached %d items in %d compact pages (%lld ms)",
             count, pages, (long long)((esp_timer_get_time() - started) / 1000));
    return true;
}

static void clean_item_cache_for_boot(const char *dir)
{
    char index_path[128];
    snprintf(index_path, sizeof(index_path), "%s/items-index.txt", dir);
    char *manifest = storage_read_file(index_path, NULL);
    unsigned version = 0, generation = 0;
    int pages = -1, count = -1;
    bool valid = manifest && sscanf(manifest, "%u %x %d %d", &version, &generation, &pages, &count) == 4 &&
                 version == 2 && pages >= 0 && pages <= ABS_MAX_LIBRARY_BOOKS / ABS_BOOK_PAGE_SIZE + 1 &&
                 count >= 0 && count <= ABS_MAX_LIBRARY_BOOKS;
    free(manifest);
    remove_old_item_pages(dir, generation, valid);
}

static bool load_paged_cache(const char *dir, abs_book_t **out_books, int *out_count)
{
    char index_path[128];
    snprintf(index_path, sizeof(index_path), "%s/items-index.txt", dir);
    char *manifest = storage_read_file(index_path, NULL);
    if (!manifest) return false;
    unsigned version = 0, generation = 0;
    int pages = -1, expected_count = -1;
    bool valid = sscanf(manifest, "%u %x %d %d", &version, &generation, &pages, &expected_count) == 4 &&
                 version == 2 && pages >= 0 && pages <= ABS_MAX_LIBRARY_BOOKS / ABS_BOOK_PAGE_SIZE + 1 &&
                 expected_count >= 0 && expected_count <= ABS_MAX_LIBRARY_BOOKS;
    free(manifest);
    if (!valid) return false;

    abs_book_t *books = NULL;
    int count = 0, capacity = 0;
    for (int page = 0; page < pages; ++page) {
        char path[160];
        snprintf(path, sizeof(path), "%s/items-%08lx-%03d.json", dir, (unsigned long)generation, page);
        char *json = storage_read_file(path, NULL);
        abs_book_t *page_books = NULL;
        int page_count = 0;
        esp_err_t err = json ? abs_parse_cached_books(json, &page_books, &page_count) : ESP_FAIL;
        free(json);
        if (err == ESP_OK) err = abs_append_books(&books, &count, &capacity, page_books, page_count);
        if (err != ESP_OK) {
            abs_free_books(page_books, page_count);
            abs_free_books(books, count);
            return false;
        }
    }
    if (count != expected_count) {
        abs_free_books(books, count);
        return false;
    }
    abs_sort_books(books, count);
    *out_books = books;
    *out_count = count;
    return true;
}

bool catalog_load_cached(void)
{
    if (!s_catalog_lock) return false;
    xSemaphoreTake(s_catalog_lock, portMAX_DELAY);
    if (s_account_switch_pending) {
        xSemaphoreGive(s_catalog_lock);
        return false;
    }
    char dir[96], path[128];
    catalog_dir(dir, sizeof(dir));
    if (!dir[0]) {
        xSemaphoreGive(s_catalog_lock);
        return false;
    }
    abs_book_t *fresh = NULL;
    int n = 0;
    bool ok = load_paged_cache(dir, &fresh, &n);
    if (!ok) {
        snprintf(path, sizeof(path), "%s/items.json", dir);
        char *items = storage_read_file(path, NULL);
        char *me = storage_read_file(ME_JSON, NULL);
        ok = items && abs_parse_books(items, me, &fresh, &n) == ESP_OK;
        free(items);
        free(me);
    }
    if (!ok) {
        abs_free_books(fresh, n);
        xSemaphoreGive(s_catalog_lock);
        return false;
    }
    download_sync_progress(fresh, n, false);
    ESP_LOGI(TAG, "showing %d items of %s from the onboard flash cache", n, abs_library_name());
    show(fresh, n, true);
    xSemaphoreGive(s_catalog_lock);
    return true;
}

bool catalog_load_network(bool quiet)
{
    if (!s_catalog_lock) return false;
    xSemaphoreTake(s_catalog_lock, portMAX_DELAY);
    if (s_account_switch_pending) {
        xSemaphoreGive(s_catalog_lock);
        return false;
    }
    char *json = NULL;
    abs_library_t *libs = NULL;
    int nl = 0;
    if (abs_get_libraries(&libs, &nl, &json) != ESP_OK || nl == 0) {
        free(json);
        free(libs);
        if (!quiet) ui_show_message_locked("Couldn't reach Audiobookshelf.\nTry Refresh in Settings.");
        xSemaphoreGive(s_catalog_lock);
        return false;
    }
    if (storage_ready()) storage_write_file(LIBRARIES_JSON, json, strlen(json));
    free(json);
    set_libraries(libs, nl);

    abs_book_t *fresh = NULL;
    int n = 0;
    char dir[96];
    catalog_dir(dir, sizeof(dir));
    book_cache_writer_t writer = {.generation = esp_random()};
    if (dir[0]) {
        strlcpy(writer.dir, dir, sizeof(writer.dir));
        (void)storage_mkdirs(dir);
        clean_item_cache_for_boot(dir);
    }
    abs_set_work_callback(pump_catalog_ui);
    const esp_err_t fetch_error = abs_get_books(s_library, &fresh, &n);
    abs_set_work_callback(NULL);
    if (fetch_error != ESP_OK) {
        ESP_LOGW(TAG, "Audiobookshelf catalog fetch stopped: %s after %d books",
                 esp_err_to_name(fetch_error), n);
        abs_free_books(fresh, n);
        if (!quiet) ui_show_message_locked("Couldn't load the library.\nTry Refresh in Settings.");
        xSemaphoreGive(s_catalog_lock);
        return false;
    }
    download_sync_progress(fresh, n, true);
    show(fresh, n, false);
    if (dir[0] && !finish_book_cache(&writer, fresh, n)) {
        ESP_LOGW(TAG, "library loaded, but the refreshed offline cache was not committed");
    }
    xSemaphoreGive(s_catalog_lock);
    return true;
}
