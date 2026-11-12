#include "abs_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "config.h"
#include "text.h"

static const char *TAG = "abs";

// The server's reverse proxy rejects some default user agents.
#define USER_AGENT "ChapterPlayer-ESP32/0.1"
#define DEVICE_NAME "Chapter Player ESP32"

// "Bearer <token>", replaced when the access token is refreshed; s_auth_gen counts refreshes.
static char *s_auth;
static SemaphoreHandle_t s_auth_lock, s_refresh_lock;
static volatile uint32_t s_auth_gen;
static volatile bool s_signed_out;
static char s_base[128];  // server base URL, no trailing slash
static char s_device_id[24];
static char s_library_name[64];
static SemaphoreHandle_t s_sync_lock;           // see sync_request()
static esp_http_client_handle_t s_sync_conn;
static char s_device_id_dl[28];
struct abs_stream {
    esp_http_client_handle_t client;
};
static abs_work_callback_t s_work_callback;

void abs_set_work_callback(abs_work_callback_t callback)
{
    s_work_callback = callback;
}

static void yield_work(void)
{
    if (s_work_callback) s_work_callback();
}

// Library listings are hundreds of KB of JSON; keep cJSON's many small nodes out of internal RAM.
static void *psram_malloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
}

static char *psram_strdup(const char *s)
{
    size_t n = strlen(s ? s : "") + 1;
    char *d = psram_malloc(n);
    if (d) memcpy(d, s ? s : "", n);
    return d;
}

void abs_api_init(void)
{
    s_sync_lock = xSemaphoreCreateMutex();
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    s_auth_lock = xSemaphoreCreateMutex();
    s_refresh_lock = xSemaphoreCreateMutex();
    s_auth = heap_caps_malloc(1100, MALLOC_CAP_SPIRAM);
    snprintf(s_auth, 1100, "Bearer %s", config_get()->access);
    strlcpy(s_base, config_get()->server, sizeof(s_base));
    size_t n = strlen(s_base);
    if (n && s_base[n - 1] == '/') s_base[n - 1] = 0;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "esp32-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_device_id_dl, sizeof(s_device_id_dl), "%s-dl", s_device_id);
}

static esp_http_client_handle_t new_client(const char *path, esp_http_client_method_t method)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", s_base, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .user_agent = USER_AGENT,
        .keep_alive_enable = true,
    };
    return esp_http_client_init(&cfg);
}

// Sets the current Authorization header (tokens can change between requests on a kept connection).
static uint32_t set_auth(esp_http_client_handle_t c)
{
    xSemaphoreTake(s_auth_lock, portMAX_DELAY);
    esp_http_client_set_header(c, "Authorization", s_auth);
    uint32_t gen = s_auth_gen;
    xSemaphoreGive(s_auth_lock);
    return gen;
}

static bool try_refresh(uint32_t failed_gen);

// Performs a request and returns the body (NUL-terminated, PSRAM) in *out and its length in *out_len.
// With `keep`, the connection is reused across calls (skipping a multi-second TLS handshake each
// time); *keep must only be used from one task. Returns HTTP status or -1.
static int request_once(esp_http_client_method_t method, const char *path, const char *body, char **out,
                        size_t *out_len, esp_http_client_handle_t *keep, uint32_t *gen)
{
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    const bool reused = keep && *keep;
    esp_http_client_handle_t c;
    if (reused) {
        char url[256];
        snprintf(url, sizeof(url), "%s%s", s_base, path);
        c = *keep;
        esp_http_client_set_url(c, url);
        esp_http_client_set_method(c, method);
    } else {
        c = new_client(path, method);
        if (!c) return -1;
    }
    *gen = set_auth(c);
    int body_len = body ? strlen(body) : 0;
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }
    int status = -1;
    bool complete = false;
    esp_err_t err = esp_http_client_open(c, body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: connect failed: %s", path, esp_err_to_name(err));
        goto done;
    }
    if (body_len && esp_http_client_write(c, body, body_len) != body_len) {
        ESP_LOGE(TAG, "%s: write failed", path);
        goto done;
    }
    if (esp_http_client_fetch_headers(c) < 0) {
        goto done;
    }
    status = esp_http_client_get_status_code(c);

    size_t cap = 16 * 1024, len = 0;
    char *buf = psram_malloc(cap);
    while (buf) {
        if (cap - len < 4097) {
            char *nb = heap_caps_realloc(buf, cap * 2, MALLOC_CAP_SPIRAM);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2;
        }
        int n = esp_http_client_read(c, buf + len, 4096);
        if (n <= 0) break;
        len += n;
    }
    complete = esp_http_client_is_complete_data_received(c);
    if (buf) {
        buf[len] = 0;
        if (out_len) *out_len = len;
        if (out) *out = buf; else free(buf);
    } else {
        ESP_LOGE(TAG, "%s: out of memory reading response (PSRAM free %u, largest block %u)",
                 path, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        status = -1;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "%s %s -> %d", method == HTTP_METHOD_GET ? "GET" : (method == HTTP_METHOD_POST ? "POST" : "PATCH"),
                 path, status);
    }
done:
    if (keep && status > 0 && complete) {
        *keep = c;
        return status;
    }
    esp_http_client_cleanup(c);
    if (keep) *keep = NULL;
    if (reused && status < 0) {
        // The server probably dropped the idle connection; try once on a fresh one.
        return request_once(method, path, body, out, out_len, keep, gen);
    }
    return status;
}

// A request, retried once with a renewed access token if the server says the token has expired.
static int request_len(esp_http_client_method_t method, const char *path, const char *body, char **out,
                       size_t *out_len, esp_http_client_handle_t *keep)
{
    uint32_t gen = 0;
    int status = request_once(method, path, body, out, out_len, keep, &gen);
    if (status == 401 && try_refresh(gen)) {
        if (out) free(*out);
        status = request_once(method, path, body, out, out_len, keep, &gen);
    }
    return status;
}

static int request(esp_http_client_method_t method, const char *path, const char *body, char **out)
{
    return request_len(method, path, body, out, NULL, NULL);
}

// Progress sync, session close and progress PATCH are small and frequent: they share one kept-open
// connection (saving a multi-second TLS handshake each time), serialised because several tasks
// may call them.
static int sync_request(esp_http_client_method_t method, const char *path, const char *body)
{
    xSemaphoreTake(s_sync_lock, portMAX_DELAY);
    int status = request_len(method, path, body, NULL, NULL, &s_sync_conn);
    xSemaphoreGive(s_sync_lock);
    return status;
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double json_num(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static const cJSON *series_sequence_value(const cJSON *series)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(series, "sequence");
    if (!value) value = cJSON_GetObjectItemCaseSensitive(series, "seriesSequence");
    return value;
}

static void set_sequence(char *out, size_t len, const cJSON *value)
{
    out[0] = 0;
    if (cJSON_IsString(value)) {
        strlcpy(out, value->valuestring, len);
    } else if (cJSON_IsNumber(value)) {
        snprintf(out, len, "%.3f", value->valuedouble);
    }
    if (strchr(out, '.')) {
        size_t n = strlen(out);
        while (n && out[n - 1] == '0') out[--n] = 0;
        if (n && out[n - 1] == '.') out[--n] = 0;
    }
}

static void parse_primary_series(const cJSON *metadata, abs_book_t *book)
{
    const cJSON *series = cJSON_GetObjectItemCaseSensitive(metadata, "series");
    if (cJSON_IsArray(series)) series = cJSON_GetArrayItem(series, 0);
    if (cJSON_IsObject(series)) {
        strlcpy(book->series_id,
                json_str(series, "id") ?: json_str(series, "seriesId") ?: "",
                sizeof(book->series_id));
        strlcpy(book->series_name,
                json_str(series, "name") ?: json_str(series, "series") ?: "",
                sizeof(book->series_name));
        set_sequence(book->series_sequence, sizeof(book->series_sequence), series_sequence_value(series));
    } else if (cJSON_IsString(series)) {
        strlcpy(book->series_name, series->valuestring, sizeof(book->series_name));
    }
    if (!book->series_name[0]) {
        strlcpy(book->series_name, json_str(metadata, "seriesName") ?: "", sizeof(book->series_name));
    }

    // Some ABS metadata stores multiple series in one legacy label, for example
    // "Ranger's Apprentice #10, Ranger's Apprentice (published order)". Keep the
    // first listed series as primary even if the series endpoint cannot be reached.
    char *additional = strstr(book->series_name, ", ");
    if (additional) *additional = 0;
    size_t name_len = strlen(book->series_name);
    while (name_len && (book->series_name[name_len - 1] == ' ' || book->series_name[name_len - 1] == '\t')) {
        book->series_name[--name_len] = 0;
    }
    char *hash = strrchr(book->series_name, '#');
    if (hash && hash > book->series_name && (hash[-1] == ' ' || hash[-1] == '\t')) {
        const char *number = hash + 1;
        while (*number == ' ' || *number == '\t') ++number;
        char parsed[24] = "";
        size_t n = 0;
        while (((*number >= '0' && *number <= '9') || *number == '.') && n + 1 < sizeof(parsed)) {
            parsed[n++] = *number++;
        }
        if (n && (*number == 0)) {
            parsed[n] = 0;
            while (n && parsed[n - 1] == '0' && strchr(parsed, '.')) parsed[--n] = 0;
            if (n && parsed[n - 1] == '.') parsed[--n] = 0;
            if (!book->series_sequence[0]) strlcpy(book->series_sequence, parsed, sizeof(book->series_sequence));
            hash[-1] = 0;
        }
    }
}

static void append_author(abs_book_t *book, const char *name)
{
    if (!name || !name[0] || book->author_count >= 6) return;
    while (*name == ' ' || *name == '\t') ++name;
    size_t len = strlen(name);
    while (len && (name[len - 1] == ' ' || name[len - 1] == '\t')) --len;
    if (!len || len >= 64) return;
    char normalized[64];
    memcpy(normalized, name, len);
    normalized[len] = 0;
    for (int i = 0; i < book->author_count; ++i) {
        if (strcasecmp(book->authors[i], normalized) == 0) return;
    }
    char *copy = psram_strdup(normalized);
    if (copy) book->authors[book->author_count++] = copy;
}

static void parse_authors(const cJSON *metadata, abs_book_t *book)
{
    const cJSON *authors = cJSON_GetObjectItemCaseSensitive(metadata, "authors");
    const cJSON *author;
    cJSON_ArrayForEach(author, authors) {
        const char *name = cJSON_IsString(author) ? author->valuestring
                         : json_str(author, "name") ?: json_str(author, "author");
        append_author(book, name);
    }
    if (!book->author_count && book->author && book->author[0]) {
        // ABS commonly returns authorName as a comma-separated label. Preserve every listed
        // author for grouping, while the display continues to use the same label as the Pi.
        char copy[512];
        strlcpy(copy, book->author, sizeof(copy));
        char *save = NULL;
        char *part = strtok_r(copy, ",;", &save);
        while (part && book->author_count < 6) {
            append_author(book, part);
            part = strtok_r(NULL, ",;", &save);
        }
    }
}

static int book_cmp(const void *pa, const void *pb)
{
    const abs_book_t *a = pa, *b = pb;
    return text_cmp(a->sort_title, b->sort_title);
}

typedef struct {
    char item_id[40];
    char episode_id[40];
    double current_time;
    double progress;
    double last_update;
    bool finished;
} abs_progress_entry_t;

static int progress_cmp(const void *pa, const void *pb)
{
    const abs_progress_entry_t *a = pa, *b = pb;
    const int id_cmp = strcmp(a->item_id, b->item_id);
    if (id_cmp) return id_cmp;
    return a->last_update < b->last_update ? -1 : (a->last_update > b->last_update ? 1 : 0);
}

static esp_err_t parse_progress(const char *me_json, abs_progress_entry_t **out, int *out_count)
{
    *out = NULL;
    *out_count = 0;
    if (!me_json) return ESP_OK;
    cJSON *root = cJSON_Parse(me_json);
    if (!root) return ESP_FAIL;
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(root, "mediaProgress");
    int count = cJSON_GetArraySize(rows);
    abs_progress_entry_t *entries = heap_caps_calloc(count ? count : 1, sizeof(*entries), MALLOC_CAP_SPIRAM);
    if (!entries) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        const char *item = json_str(row, "libraryItemId");
        if (!item) continue;
        abs_progress_entry_t *entry = &entries[(*out_count)++];
        strlcpy(entry->item_id, item, sizeof(entry->item_id));
        strlcpy(entry->episode_id, json_str(row, "episodeId") ?: "", sizeof(entry->episode_id));
        entry->current_time = json_num(row, "currentTime");
        entry->progress = json_num(row, "progress");
        entry->last_update = json_num(row, "lastUpdate");
        entry->finished = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "isFinished"));
    }
    cJSON_Delete(root);
    if (*out_count > 1) qsort(entries, *out_count, sizeof(*entries), progress_cmp);
    *out = entries;
    return ESP_OK;
}

static void apply_progress(abs_book_t *books, int count, const abs_progress_entry_t *entries, int entry_count)
{
    for (int i = 0; i < count; ++i) {
        abs_book_t *book = &books[i];
        int low = 0, high = entry_count;
        while (low < high) {
            int mid = low + (high - low) / 2;
            if (strcmp(entries[mid].item_id, book->id) < 0) low = mid + 1;
            else high = mid;
        }
        for (int p = low; p < entry_count && strcmp(entries[p].item_id, book->id) == 0; ++p) {
            const abs_progress_entry_t *entry = &entries[p];
            if (!book->podcast && !entry->episode_id[0]) {
                book->current_time = entry->current_time;
                book->progress = entry->progress;
                book->finished = entry->finished;
                book->last_update = entry->last_update;
            } else if (book->podcast && entry->episode_id[0] && !entry->finished &&
                       entry->current_time > 0 && entry->last_update > book->last_update) {
                // A show is "in progress" through its most recently played unfinished episode.
                book->current_time = entry->current_time;
                book->progress = entry->progress;
                book->last_update = entry->last_update;
                strlcpy(book->resume_episode, entry->episode_id, sizeof(book->resume_episode));
            }
        }
    }
}

static esp_err_t parse_books_page(const char *items_json, const abs_progress_entry_t *progress,
                                  int progress_count, abs_book_t **out_books, int *out_count)
{
    *out_books = NULL;
    *out_count = 0;
    cJSON *root = cJSON_Parse(items_json);
    if (!root) return ESP_FAIL;
    const cJSON *results = cJSON_GetObjectItem(root, "results");
    int n = cJSON_GetArraySize(results);
    abs_book_t *books = heap_caps_calloc(n ? n : 1, sizeof(abs_book_t), MALLOC_CAP_SPIRAM);
    if (!books) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    int count = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, results) {
        const cJSON *media = cJSON_GetObjectItem(it, "media");
        const cJSON *meta = cJSON_GetObjectItem(media, "metadata");
        const char *id = json_str(it, "id");
        if (!id || !books) continue;
        abs_book_t *b = &books[count++];
        strlcpy(b->id, id, sizeof(b->id));
        b->title = psram_strdup(json_str(meta, "title") ?: "Untitled");
        b->sort_title = psram_strdup(json_str(meta, "titleIgnorePrefix") ?: b->title);
        b->added_at = json_num(it, "addedAt");
        b->podcast = strcmp(json_str(it, "mediaType") ?: "", "podcast") == 0;
        if (b->podcast) {
            b->author = psram_strdup(json_str(meta, "author") ?: "");
            b->num_episodes = (int)json_num(media, "numEpisodes");
        } else {
            b->author = psram_strdup(json_str(meta, "authorName") ?: "");
            parse_authors(meta, b);
            b->duration = json_num(media, "duration");
            parse_primary_series(meta, b);
            const cJSON *cached_series = cJSON_GetObjectItemCaseSensitive(it, "_chapterSeries");
            if (cJSON_IsObject(cached_series)) {
                strlcpy(b->series_id, json_str(cached_series, "id") ?: "", sizeof(b->series_id));
                strlcpy(b->series_name, json_str(cached_series, "name") ?: b->series_name,
                        sizeof(b->series_name));
                set_sequence(b->series_sequence, sizeof(b->series_sequence),
                             cJSON_GetObjectItemCaseSensitive(cached_series, "sequence"));
                b->series_total = (int)json_num(cached_series, "total");
            }
        }
        const cJSON *cached_progress = cJSON_GetObjectItemCaseSensitive(it, "_chapterProgress");
        if (cJSON_IsObject(cached_progress)) {
            b->current_time = json_num(cached_progress, "currentTime");
            b->progress = (float)json_num(cached_progress, "progress");
            b->finished = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cached_progress, "finished"));
            b->last_update = json_num(cached_progress, "lastUpdate");
            strlcpy(b->resume_episode, json_str(cached_progress, "episodeId") ?: "",
                    sizeof(b->resume_episode));
        }
    }
    cJSON_Delete(root);

    apply_progress(books, count, progress, progress_count);

    qsort(books, count, sizeof(abs_book_t), book_cmp);
    *out_books = books;
    *out_count = count;
    return ESP_OK;
}

esp_err_t abs_parse_books(const char *items_json, const char *me_json, abs_book_t **out_books, int *out_count)
{
    abs_progress_entry_t *progress = NULL;
    int progress_count = 0;
    esp_err_t err = parse_progress(me_json, &progress, &progress_count);
    if (err == ESP_OK) err = parse_books_page(items_json, progress, progress_count, out_books, out_count);
    free(progress);
    return err;
}

esp_err_t abs_append_books(abs_book_t **books, int *count, int *capacity, abs_book_t *page, int page_count)
{
    if (!books || !count || !capacity || page_count < 0 || *count + page_count > ABS_MAX_LIBRARY_BOOKS) {
        return ESP_ERR_INVALID_SIZE;
    }
    const int needed = *count + page_count;
    if (needed > *capacity) {
        // Avoid a large doubling allocation at the 3,200-book boundary (3,200 -> 6,400).
        // Reallocating a multi-megabyte PSRAM block while the cached catalog is still live can
        // fail even though the final catalog itself fits. Grow by at least four API pages instead.
        int next = *capacity ? *capacity : ABS_BOOK_PAGE_SIZE * 4;
        while (next < needed) {
            const int growth = next / 4 > ABS_BOOK_PAGE_SIZE * 4
                                   ? next / 4 : ABS_BOOK_PAGE_SIZE * 4;
            next = next > ABS_MAX_LIBRARY_BOOKS - growth ? ABS_MAX_LIBRARY_BOOKS : next + growth;
        }
        abs_book_t *grown = heap_caps_realloc(*books, (size_t)next * sizeof(**books), MALLOC_CAP_SPIRAM);
        if (!grown) {
            ESP_LOGE(TAG,
                     "could not grow catalog from %d to %d books (%u bytes); PSRAM free %u, largest block %u",
                     *capacity, next, (unsigned)((size_t)next * sizeof(**books)),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            return ESP_ERR_NO_MEM;
        }
        *books = grown;
        *capacity = next;
    }
    if (page_count) memcpy(*books + *count, page, (size_t)page_count * sizeof(*page));
    *count = needed;
    free(page); // The per-page strings moved with their structs into the aggregate.
    return ESP_OK;
}

void abs_sort_books(abs_book_t *books, int count)
{
    if (books && count > 1) qsort(books, count, sizeof(*books), book_cmp);
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} cache_json_writer_t;

static bool cache_reserve(cache_json_writer_t *writer, size_t extra)
{
    if (extra > SIZE_MAX - writer->length - 1) return false;
    const size_t needed = writer->length + extra + 1;
    if (needed <= writer->capacity) return true;
    size_t capacity = writer->capacity ? writer->capacity : 16384;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
        capacity *= 2;
    }
    char *grown = writer->data
        ? heap_caps_realloc(writer->data, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!grown) return false;
    writer->data = grown;
    writer->capacity = capacity;
    return true;
}

static bool cache_append_n(cache_json_writer_t *writer, const char *value, size_t length)
{
    if (!cache_reserve(writer, length)) return false;
    memcpy(writer->data + writer->length, value, length);
    writer->length += length;
    writer->data[writer->length] = 0;
    return true;
}

static bool cache_append(cache_json_writer_t *writer, const char *value)
{
    return cache_append_n(writer, value, strlen(value));
}

static bool cache_append_string(cache_json_writer_t *writer, const char *value)
{
    if (!cache_append(writer, "\"")) return false;
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    for (; *p; ++p) {
        char escaped[7];
        const char *out = (const char *)p;
        size_t length = 1;
        switch (*p) {
        case '"': out = "\\\""; length = 2; break;
        case '\\': out = "\\\\"; length = 2; break;
        case '\b': out = "\\b"; length = 2; break;
        case '\f': out = "\\f"; length = 2; break;
        case '\n': out = "\\n"; length = 2; break;
        case '\r': out = "\\r"; length = 2; break;
        case '\t': out = "\\t"; length = 2; break;
        default:
            if (*p < 0x20) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
                out = escaped;
                length = 6;
            }
            break;
        }
        if (!cache_append_n(writer, out, length)) return false;
    }
    return cache_append(writer, "\"");
}

static bool cache_append_number(cache_json_writer_t *writer, const char *format, double value)
{
    char number[32];
    const int length = snprintf(number, sizeof(number), format, value);
    return length > 0 && (size_t)length < sizeof(number) && cache_append_n(writer, number, (size_t)length);
}

char *abs_books_cache_json(const abs_book_t *books, int count)
{
    cache_json_writer_t writer = {0};
    bool ok = cache_append(&writer, "{\"v\":2,\"b\":[");
    for (int i = 0; ok && i < count; ++i) {
        const abs_book_t *book = &books[i];
        if (i) ok = cache_append(&writer, ",");
        ok = ok && cache_append(&writer, "{\"i\":") && cache_append_string(&writer, book->id) &&
             cache_append(&writer, ",\"t\":") && cache_append_string(&writer, book->title ? book->title : "Untitled") &&
             cache_append(&writer, ",\"s\":") && cache_append_string(&writer, book->sort_title ? book->sort_title : "") &&
             cache_append(&writer, ",\"a\":") && cache_append_string(&writer, book->author ? book->author : "") &&
             cache_append(&writer, ",\"A\":[");
        for (int a = 0; ok && a < book->author_count; ++a) {
            if (a) ok = cache_append(&writer, ",");
            ok = ok && cache_append_string(&writer, book->authors[a] ? book->authors[a] : "");
        }
        ok = ok && cache_append(&writer, "],\"S\":") && cache_append_string(&writer, book->series_name) &&
             cache_append(&writer, ",\"I\":") && cache_append_string(&writer, book->series_id) &&
             cache_append(&writer, ",\"q\":") && cache_append_string(&writer, book->series_sequence) &&
             cache_append(&writer, ",\"N\":") && cache_append_number(&writer, "%.0f", book->series_total) &&
             cache_append(&writer, ",\"d\":") && cache_append_number(&writer, "%.15g", book->duration) &&
             cache_append(&writer, ",\"c\":") && cache_append_number(&writer, "%.15g", book->current_time) &&
             cache_append(&writer, ",\"p\":") && cache_append_number(&writer, "%.8g", book->progress) &&
             cache_append(&writer, book->finished ? ",\"f\":true" : ",\"f\":false") &&
             cache_append(&writer, ",\"u\":") && cache_append_number(&writer, "%.15g", book->last_update) &&
             cache_append(&writer, ",\"x\":") && cache_append_number(&writer, "%.15g", book->added_at) &&
             cache_append(&writer, book->podcast ? ",\"P\":true" : ",\"P\":false") &&
             cache_append(&writer, ",\"e\":") && cache_append_number(&writer, "%.0f", book->num_episodes) &&
             cache_append(&writer, ",\"r\":") && cache_append_string(&writer, book->resume_episode) &&
             cache_append(&writer, "}");
    }
    ok = ok && cache_append(&writer, "]}");
    if (!ok) {
        free(writer.data);
        return NULL;
    }
    return writer.data;
}

esp_err_t abs_parse_cached_books(const char *json, abs_book_t **out_books, int *out_count)
{
    *out_books = NULL;
    *out_count = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_FAIL;
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "b");
    if ((int)json_num(root, "v") != 2 || !cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    int n = cJSON_GetArraySize(items);
    abs_book_t *books = heap_caps_calloc(n ? n : 1, sizeof(*books), MALLOC_CAP_SPIRAM);
    if (!books) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        const char *id = json_str(item, "i");
        if (!id) { err = ESP_ERR_INVALID_RESPONSE; break; }
        abs_book_t *book = &books[*out_count];
        strlcpy(book->id, id, sizeof(book->id));
        book->title = psram_strdup(json_str(item, "t") ?: "Untitled");
        book->sort_title = psram_strdup(json_str(item, "s") ?: book->title);
        book->author = psram_strdup(json_str(item, "a") ?: "");
        if (!book->title || !book->sort_title || !book->author) { err = ESP_ERR_NO_MEM; break; }
        strlcpy(book->series_name, json_str(item, "S") ?: "", sizeof(book->series_name));
        strlcpy(book->series_id, json_str(item, "I") ?: "", sizeof(book->series_id));
        strlcpy(book->series_sequence, json_str(item, "q") ?: "", sizeof(book->series_sequence));
        strlcpy(book->resume_episode, json_str(item, "r") ?: "", sizeof(book->resume_episode));
        book->series_total = (int)json_num(item, "N");
        book->duration = json_num(item, "d");
        book->current_time = json_num(item, "c");
        book->progress = (float)json_num(item, "p");
        book->finished = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "f"));
        book->last_update = json_num(item, "u");
        book->added_at = json_num(item, "x");
        book->podcast = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "P"));
        book->num_episodes = (int)json_num(item, "e");
        const cJSON *authors = cJSON_GetObjectItemCaseSensitive(item, "A");
        const cJSON *author;
        cJSON_ArrayForEach(author, authors) {
            if (cJSON_IsString(author)) append_author(book, author->valuestring);
        }
        ++*out_count;
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        abs_free_books(books, *out_count + 1);
        *out_count = 0;
        return err;
    }
    abs_sort_books(books, *out_count);
    *out_books = books;
    return ESP_OK;
}

esp_err_t abs_parse_libraries(const char *json, abs_library_t **out, int *count)
{
    *out = NULL;
    *count = 0;
    cJSON *root = cJSON_Parse(json);
    const cJSON *libs = cJSON_GetObjectItem(root, "libraries");
    int n = cJSON_GetArraySize(libs);
    abs_library_t *list = heap_caps_calloc(n ? n : 1, sizeof(abs_library_t), MALLOC_CAP_SPIRAM);
    const cJSON *lib;
    cJSON_ArrayForEach(lib, libs) {
        const char *id = json_str(lib, "id"), *type = json_str(lib, "mediaType");
        if (!id || !list) continue;
        abs_library_t *l = &list[(*count)++];
        strlcpy(l->id, id, sizeof(l->id));
        strlcpy(l->name, json_str(lib, "name") ?: "Library", sizeof(l->name));
        l->podcast = type && strcmp(type, "podcast") == 0;
    }
    cJSON_Delete(root);
    *out = list;
    return root ? ESP_OK : ESP_FAIL;
}

typedef struct {
    const char *id;
    int book_index;
} book_id_ref_t;

static int book_id_ref_cmp(const void *pa, const void *pb)
{
    const book_id_ref_t *a = pa, *b = pb;
    return strcmp(a->id, b->id);
}

static abs_book_t *find_book_by_id(abs_book_t *books, book_id_ref_t *refs, int count, const char *id)
{
    int low = 0, high = count;
    while (low < high) {
        int mid = low + (high - low) / 2;
        int cmp = strcmp(refs[mid].id, id);
        if (cmp < 0) low = mid + 1;
        else high = mid;
    }
    return low < count && strcmp(refs[low].id, id) == 0 ? &books[refs[low].book_index] : NULL;
}

static bool canonical_series_label(const char *label, const char *name, char *sequence, size_t length)
{
    if (!label || !name || !name[0]) return false;
    const size_t name_len = strlen(name);
    if (strncasecmp(label, name, name_len) != 0) return false;
    const char *rest = label + name_len;
    while (*rest == ' ' || *rest == '\t') ++rest;
    sequence[0] = 0;
    if (*rest == '#') {
        ++rest;
        while (*rest == ' ' || *rest == '\t') ++rest;
        size_t n = 0;
        while ((*rest >= '0' && *rest <= '9') || *rest == '.') {
            if (n + 1 < length) sequence[n++] = *rest;
            ++rest;
        }
        sequence[n] = 0;
        while (n && sequence[n - 1] == '.') sequence[--n] = 0;
        while (n && sequence[n - 1] == '0' && strchr(sequence, '.')) sequence[--n] = 0;
        if (n && sequence[n - 1] == '.') sequence[--n] = 0;
        while (*rest == ' ' || *rest == '\t') ++rest;
    }
    return *rest == 0 || *rest == ',';
}

static void merge_series_member(const char *series_id, const char *series_name, int total,
                                const char *member_sequence, abs_book_t *book)
{
    const bool id_match = series_id && book->series_id[0] && strcmp(series_id, book->series_id) == 0;
    // The metadata's series ID and the series endpoint's ID can differ. Match the canonical
    // endpoint name only for a book that is actually listed as a member of that series.
    char parsed_sequence[24] = "";
    const bool name_match = book->series_name[0] &&
        (strcasecmp(series_name, book->series_name) == 0 ||
         canonical_series_label(book->series_name, series_name, parsed_sequence, sizeof(parsed_sequence)));
    if (id_match || name_match || !book->series_name[0]) {
        strlcpy(book->series_name, series_name, sizeof(book->series_name));
        if (series_id) strlcpy(book->series_id, series_id, sizeof(book->series_id));
        if (!book->series_sequence[0]) {
            const char *sequence = member_sequence && member_sequence[0] ? member_sequence : parsed_sequence;
            if (sequence[0]) strlcpy(book->series_sequence, sequence, sizeof(book->series_sequence));
        }
        book->series_total = total;
    }
}

static void merge_series_row(const cJSON *row, abs_book_t *books, book_id_ref_t *refs, int book_count)
{
    const char *series_id = json_str(row, "id");
    const char *series_name = json_str(row, "name");
    if (!series_name || !series_name[0]) return;
    int total = (int)json_num(row, "numBooks");
    const cJSON *ids = cJSON_GetObjectItemCaseSensitive(row, "libraryItemIds");
    const cJSON *members = cJSON_GetObjectItemCaseSensitive(row, "books");
    if (!cJSON_IsArray(members)) members = cJSON_GetObjectItemCaseSensitive(row, "items");
    int member_count = cJSON_GetArraySize(ids);
    if (member_count == 0) member_count = cJSON_GetArraySize(members);
    if (total < member_count) total = member_count;

    if (cJSON_IsArray(members) && cJSON_GetArraySize(members) > 0) {
        const cJSON *member;
        cJSON_ArrayForEach(member, members) {
            const cJSON *item = cJSON_GetObjectItemCaseSensitive(member, "libraryItem");
            if (!cJSON_IsObject(item)) item = cJSON_GetObjectItemCaseSensitive(member, "item");
            if (!cJSON_IsObject(item)) item = member;
            const char *item_id = json_str(item, "id");
            if (!item_id) item_id = json_str(member, "libraryItemId");
            if (!item_id) item_id = json_str(member, "itemId");
            if (!item_id) continue;
            abs_book_t *book = find_book_by_id(books, refs, book_count, item_id);
            if (!book) continue;
            char sequence[24] = "";
            const cJSON *seq = cJSON_GetObjectItemCaseSensitive(member, "seriesSequence");
            if (!seq) seq = series_sequence_value(member);
            if (!seq) seq = series_sequence_value(item);
            set_sequence(sequence, sizeof(sequence), seq);
            merge_series_member(series_id, series_name, total, sequence, book);
        }
    } else {
        const cJSON *id_item;
        cJSON_ArrayForEach(id_item, ids) {
            if (!cJSON_IsString(id_item)) continue;
            abs_book_t *book = find_book_by_id(books, refs, book_count, id_item->valuestring);
            if (book) merge_series_member(series_id, series_name, total, "", book);
        }
    }
}

static void fetch_series_catalog(const char *library_id, abs_book_t *books, int book_count)
{
    // Series pages include member records, so keep them smaller than item pages to avoid a
    // multi-megabyte response when a library has thousands of books.
    enum { SERIES_PAGE_SIZE = 25 };
    book_id_ref_t *refs = heap_caps_malloc((size_t)(book_count ? book_count : 1) * sizeof(*refs),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!refs) {
        ESP_LOGW(TAG, "not enough memory to reconcile series metadata; keeping item metadata");
        return;
    }
    for (int i = 0; i < book_count; ++i) refs[i] = (book_id_ref_t){books[i].id, i};
    qsort(refs, book_count, sizeof(*refs), book_id_ref_cmp);

    // Read all series pages, reusing the HTTP connection and matching each member by ID.
    esp_http_client_handle_t connection = NULL;
    int series_count = 0;
    for (int page = 0; page <= ABS_MAX_LIBRARY_BOOKS / SERIES_PAGE_SIZE; ++page) {
        char path[160], *json = NULL;
        snprintf(path, sizeof(path), "/api/libraries/%s/series?limit=%d&page=%d",
                 library_id, SERIES_PAGE_SIZE, page);
        if (request_len(HTTP_METHOD_GET, path, NULL, &json, NULL, &connection) != 200 || !json) {
            free(json);
            ESP_LOGW(TAG, "series page %d unavailable; using item metadata for remaining books", page + 1);
            break;
        }
        cJSON *root = cJSON_Parse(json);
        free(json);
        if (!root) break;
        const cJSON *rows = cJSON_GetObjectItemCaseSensitive(root, "results");
        if (!cJSON_IsArray(rows)) rows = root;
        const int count = cJSON_GetArraySize(rows);
        const int total = (int)json_num(root, "total");
        const cJSON *row;
        cJSON_ArrayForEach(row, rows) merge_series_row(row, books, refs, book_count);
        cJSON_Delete(root);
        series_count += count;
        yield_work();
        if (!count || (count < SERIES_PAGE_SIZE && (!total || (page + 1) * SERIES_PAGE_SIZE >= total))) break;
    }
    ESP_LOGI(TAG, "series catalog: reconciled %d series rows", series_count);
    if (connection) esp_http_client_cleanup(connection);
    free(refs);
}

static int sequence_ceiling(const char *sequence)
{
    if (!sequence || !sequence[0]) return 0;
    char *end = NULL;
    const float value = strtof(sequence, &end);
    if (end == sequence || *end || value <= 0) return 0;
    const int whole = (int)value;
    return whole + (value > whole ? 1 : 0);
}

typedef struct {
    const char *name;
    int book_index;
} series_total_ref_t;

static int series_total_ref_cmp(const void *pa, const void *pb)
{
    const series_total_ref_t *a = pa, *b = pb;
    return strcasecmp(a->name, b->name);
}

static void fill_series_totals(abs_book_t *books, int book_count)
{
    series_total_ref_t *refs = heap_caps_malloc((size_t)(book_count ? book_count : 1) * sizeof(*refs),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!refs) return;
    int n = 0;
    for (int i = 0; i < book_count; ++i) {
        if (books[i].series_name[0]) refs[n++] = (series_total_ref_t){books[i].series_name, i};
    }
    qsort(refs, n, sizeof(*refs), series_total_ref_cmp);
    for (int start = 0; start < n;) {
        int end = start + 1, highest_sequence = 0;
        while (end < n && strcasecmp(refs[start].name, refs[end].name) == 0) ++end;
        for (int j = start; j < end; ++j) {
            const int ceiling = sequence_ceiling(books[refs[j].book_index].series_sequence);
            if (ceiling > highest_sequence) highest_sequence = ceiling;
        }
        const int total = end - start > highest_sequence ? end - start : highest_sequence;
        for (int j = start; j < end; ++j) {
            abs_book_t *book = &books[refs[j].book_index];
            if (book->series_total < total) book->series_total = total;
        }
        start = end;
    }
    free(refs);
}

esp_err_t abs_get_libraries(abs_library_t **out, int *count, char **json)
{
    *out = NULL;
    *count = 0;
    *json = NULL;
    if (request(HTTP_METHOD_GET, "/api/libraries", NULL, json) != 200) {
        free(*json);
        *json = NULL;
        return ESP_FAIL;
    }
    return abs_parse_libraries(*json, out, count);
}

esp_err_t abs_get_books(const char *library_id, abs_book_t **out_books, int *out_count)
{
    *out_books = NULL;
    *out_count = 0;
    abs_progress_entry_t *progress = NULL;
    int progress_count = 0;
    char *me_json = NULL;
    if (request(HTTP_METHOD_GET, "/api/me", NULL, &me_json) != 200 ||
        parse_progress(me_json, &progress, &progress_count) != ESP_OK) {
        free(me_json);
        free(progress);
        return ESP_FAIL;
    }

    free(me_json);
    int capacity = 0;
    esp_err_t err = ESP_OK;
    esp_http_client_handle_t page_conn = NULL;
    for (int page = 0; page <= ABS_MAX_LIBRARY_BOOKS / ABS_BOOK_PAGE_SIZE; ++page) {
        char path[192], *page_json = NULL;
        snprintf(path, sizeof(path), "/api/libraries/%s/items?limit=%d&page=%d&minified=1",
                 library_id, ABS_BOOK_PAGE_SIZE, page);
        const int status = request_len(HTTP_METHOD_GET, path, NULL, &page_json, NULL, &page_conn);
        if (status != 200 || !page_json) {
            free(page_json);
            ESP_LOGW(TAG, "library page %d request failed (HTTP %d)", page + 1, status);
            err = ESP_FAIL;
            break;
        }
        abs_book_t *page_books = NULL;
        int page_count = 0;
        err = parse_books_page(page_json, progress, progress_count, &page_books, &page_count);
        if (err != ESP_OK) {
            free(page_json);
            abs_free_books(page_books, page_count);
            ESP_LOGW(TAG, "library page %d could not be parsed: %s", page + 1, esp_err_to_name(err));
            break;
        }
        if (page_count == 0) {
            free(page_json);
            free(page_books);
            yield_work();
            break;
        }
        if (*out_count + page_count > ABS_MAX_LIBRARY_BOOKS) {
            ESP_LOGE(TAG, "library exceeds the %d-item firmware limit", ABS_MAX_LIBRARY_BOOKS);
            free(page_json);
            abs_free_books(page_books, page_count);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        ESP_LOGI(TAG, "library page %d: %d items", page + 1, page_count);
        free(page_json);
        err = abs_append_books(out_books, out_count, &capacity, page_books, page_count);
        if (err != ESP_OK) {
            abs_free_books(page_books, page_count);
            break;
        }
        yield_work();
        if (page_count < ABS_BOOK_PAGE_SIZE) break;
    }
    if (page_conn) esp_http_client_cleanup(page_conn);
    free(progress);
    if (err != ESP_OK) {
        abs_free_books(*out_books, *out_count);
        *out_books = NULL;
        *out_count = 0;
        return err;
    }
    if (*out_count > 0) {
        fetch_series_catalog(library_id, *out_books, *out_count);
        fill_series_totals(*out_books, *out_count);
        abs_sort_books(*out_books, *out_count);
    }
    ESP_LOGI(TAG, "loaded %d items", *out_count);
    return ESP_OK;
}

/* ---------- podcast episodes ---------- */

static int episode_cmp(const void *pa, const void *pb)
{
    const abs_episode_t *a = pa, *b = pb;
    bool ai = a->current_time > 0 && !a->finished, bi = b->current_time > 0 && !b->finished;
    if (ai != bi) return ai ? -1 : 1;
    if (ai && a->last_update != b->last_update) return a->last_update > b->last_update ? -1 : 1;
    return a->published_at > b->published_at ? -1 : (a->published_at < b->published_at ? 1 : 0);
}

esp_err_t abs_get_episodes(const char *item_id, abs_episode_t **out, int *count)
{
    *out = NULL;
    *count = 0;
    char path[96], *body = NULL;
    snprintf(path, sizeof(path), "/api/items/%s?expanded=1", item_id);
    if (request(HTTP_METHOD_GET, path, NULL, &body) != 200) {
        free(body);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const cJSON *episodes = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "media"), "episodes");
    int n = cJSON_GetArraySize(episodes);
    abs_episode_t *eps = heap_caps_calloc(n ? n : 1, sizeof(abs_episode_t), MALLOC_CAP_SPIRAM);
    const cJSON *e;
    cJSON_ArrayForEach(e, episodes) {
        const char *id = json_str(e, "id");
        if (!id || !eps) continue;
        abs_episode_t *ep = &eps[(*count)++];
        strlcpy(ep->id, id, sizeof(ep->id));
        ep->title = psram_strdup(json_str(e, "title") ?: "Episode");
        ep->duration = json_num(e, "duration");
        ep->published_at = json_num(e, "publishedAt");
    }
    cJSON_Delete(root);

    // Per-episode progress from the user record.
    if (request(HTTP_METHOD_GET, "/api/me", NULL, &body) == 200) {
        root = cJSON_Parse(body);
        const cJSON *p;
        cJSON_ArrayForEach(p, cJSON_GetObjectItem(root, "mediaProgress")) {
            const char *item = json_str(p, "libraryItemId"), *episode = json_str(p, "episodeId");
            if (!item || !episode || strcmp(item, item_id) != 0) continue;
            for (int i = 0; i < *count; i++) {
                if (strcmp(eps[i].id, episode) == 0) {
                    eps[i].current_time = json_num(p, "currentTime");
                    eps[i].progress = json_num(p, "progress");
                    eps[i].finished = cJSON_IsTrue(cJSON_GetObjectItem(p, "isFinished"));
                    eps[i].last_update = json_num(p, "lastUpdate");
                    break;
                }
            }
        }
        cJSON_Delete(root);
    }
    free(body);
    qsort(eps, *count, sizeof(abs_episode_t), episode_cmp);
    *out = eps;
    return ESP_OK;
}

void abs_free_episodes(abs_episode_t *eps, int count)
{
    for (int i = 0; i < count; i++) free(eps[i].title);
    free(eps);
}

char *abs_episodes_to_json(const abs_episode_t *eps, int count)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", eps[i].id);
        cJSON_AddStringToObject(o, "t", eps[i].title);
        cJSON_AddNumberToObject(o, "d", eps[i].duration);
        cJSON_AddNumberToObject(o, "p", eps[i].published_at);
        cJSON_AddNumberToObject(o, "c", eps[i].current_time);
        cJSON_AddNumberToObject(o, "g", eps[i].progress);
        cJSON_AddBoolToObject(o, "f", eps[i].finished);
        cJSON_AddNumberToObject(o, "u", eps[i].last_update);
        cJSON_AddItemToArray(arr, o);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

esp_err_t abs_episodes_from_json(const char *json, abs_episode_t **out, int *count)
{
    *out = NULL;
    *count = 0;
    cJSON *arr = cJSON_Parse(json);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return ESP_FAIL;
    }
    int n = cJSON_GetArraySize(arr);
    abs_episode_t *eps = heap_caps_calloc(n ? n : 1, sizeof(abs_episode_t), MALLOC_CAP_SPIRAM);
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        const char *id = json_str(o, "id");
        if (!id || !eps) continue;
        abs_episode_t *ep = &eps[(*count)++];
        strlcpy(ep->id, id, sizeof(ep->id));
        ep->title = psram_strdup(json_str(o, "t") ?: "Episode");
        ep->duration = json_num(o, "d");
        ep->published_at = json_num(o, "p");
        ep->current_time = json_num(o, "c");
        ep->progress = json_num(o, "g");
        ep->finished = cJSON_IsTrue(cJSON_GetObjectItem(o, "f"));
        ep->last_update = json_num(o, "u");
    }
    cJSON_Delete(arr);
    *out = eps;
    return ESP_OK;
}

const char *abs_library_name(void)
{
    return s_library_name;
}

void abs_set_library_name(const char *name)
{
    strlcpy(s_library_name, name ? name : "", sizeof(s_library_name));
}

const char *abs_server(void)
{
    const char *p = strstr(s_base, "://");
    return p ? p + 3 : s_base;
}

esp_err_t abs_patch_progress(const char *item_id, double current_time, double duration, bool finished)
{
    char path[96], body[160];
    snprintf(path, sizeof(path), "/api/me/progress/%s", item_id);
    snprintf(body, sizeof(body), "{\"currentTime\":%.2f,\"duration\":%.2f,\"progress\":%.5f,\"isFinished\":%s}",
             current_time, duration, duration > 0 ? current_time / duration : 0, finished ? "true" : "false");
    return sync_request(HTTP_METHOD_PATCH, path, body) == 200 ? ESP_OK : ESP_FAIL;
}

void abs_free_books(abs_book_t *books, int count)
{
    for (int i = 0; i < count; i++) {
        free(books[i].title);
        free(books[i].sort_title);
        free(books[i].author);
        for (int a = 0; a < books[i].author_count; ++a) free(books[i].authors[a]);
    }
    free(books);
}

esp_err_t abs_start_session(const char *item_id, const char *episode_id, bool for_download, abs_session_t *out)
{
    memset(out, 0, sizeof(*out));
    char path[140], req[320];
    if (episode_id && episode_id[0]) {
        snprintf(path, sizeof(path), "/api/items/%s/play/%s", item_id, episode_id);
    } else {
        snprintf(path, sizeof(path), "/api/items/%s/play", item_id);
    }
    // Only advertising audio/mpeg plus forceTranscode makes the server hand back an HLS stream of
    // MPEG-TS segments (AAC, or MP3 for MP3 sources) regardless of the book's file format.
    snprintf(req, sizeof(req),
             "{\"deviceInfo\":{\"clientName\":\"" DEVICE_NAME "\",\"deviceId\":\"%s\"},"
             "\"supportedMimeTypes\":[\"audio/mpeg\"],\"mediaPlayer\":\"esp32\",\"forceTranscode\":true}",
             // Downloads use their own device id so the server never treats them as the same
             // session as playback (it may close one device's session when another starts).
             for_download ? s_device_id_dl : s_device_id);
    char *body = NULL;
    if (request(HTTP_METHOD_POST, path, req, &body) != 200) {
        free(body);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const char *id = json_str(root, "id");
    const cJSON *track = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "audioTracks"), 0);
    const char *url = json_str(track, "contentUrl");
    if (!id || !url || strncmp(url, "/hls/", 5) != 0) {
        ESP_LOGE(TAG, "unexpected play response (playMethod %d)", (int)json_num(root, "playMethod"));
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    strlcpy(out->id, id, sizeof(out->id));
    strlcpy(out->hls_path, url, sizeof(out->hls_path));
    out->current_time = json_num(root, "currentTime");
    out->duration = json_num(root, "duration");
    strlcpy(out->display_title, json_str(root, "displayTitle") ?: "", sizeof(out->display_title));
    strlcpy(out->display_author, json_str(root, "displayAuthor") ?: "", sizeof(out->display_author));

    const cJSON *chapters = cJSON_GetObjectItem(root, "chapters");
    int n = cJSON_GetArraySize(chapters);
    if (n > 0) {
        out->chapters = heap_caps_calloc(n, sizeof(abs_chapter_t), MALLOC_CAP_SPIRAM);
        const cJSON *ch;
        cJSON_ArrayForEach(ch, chapters) {
            if (!out->chapters) break;
            abs_chapter_t *c = &out->chapters[out->chapter_count++];
            c->title = psram_strdup(json_str(ch, "title") ?: "");
            c->start = json_num(ch, "start");
            c->end = json_num(ch, "end");
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "session %s at %.0f/%.0f s, %d chapters", out->id, out->current_time, out->duration,
             out->chapter_count);
    return ESP_OK;
}

void abs_free_session(abs_session_t *s)
{
    for (int i = 0; i < s->chapter_count; i++) {
        free(s->chapters[i].title);
    }
    free(s->chapters);
    memset(s, 0, sizeof(*s));
}

esp_err_t abs_sync_session(const char *session_id, double current_time, double time_listening, double duration)
{
    char path[96], body[160];
    snprintf(path, sizeof(path), "/api/session/%s/sync", session_id);
    snprintf(body, sizeof(body), "{\"currentTime\":%.2f,\"timeListening\":%.2f,\"duration\":%.2f}",
             current_time, time_listening, duration);
    return sync_request(HTTP_METHOD_POST, path, body) == 200 ? ESP_OK : ESP_FAIL;
}

esp_err_t abs_close_session(const char *session_id, double current_time, double time_listening)
{
    char path[96], body[128];
    snprintf(path, sizeof(path), "/api/session/%s/close", session_id);
    if (time_listening > 0) {
        snprintf(body, sizeof(body), "{\"currentTime\":%.2f,\"timeListening\":%.2f}", current_time, time_listening);
    } else {
        // Nothing was played; don't let the close create or move a progress record.
        strcpy(body, "{}");
    }
    return sync_request(HTTP_METHOD_POST, path, body) == 200 ? ESP_OK : ESP_FAIL;
}

abs_stream_t *abs_stream_create(void)
{
    return heap_caps_calloc(1, sizeof(abs_stream_t), MALLOC_CAP_SPIRAM);
}

int abs_stream_begin(abs_stream_t *st, const char *path)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", s_base, path);
    if (!st->client) {
        st->client = new_client(path, HTTP_METHOD_GET);
        if (!st->client) return -1;
    } else {
        esp_http_client_set_url(st->client, url);
    }
    const uint32_t gen = set_auth(st->client);
    esp_err_t err = esp_http_client_open(st->client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "segment connect failed: %s", esp_err_to_name(err));
        abs_stream_end(st, false);
        return -1;
    }
    if (esp_http_client_fetch_headers(st->client) < 0) {
        abs_stream_end(st, false);
        return -1;
    }
    int status = esp_http_client_get_status_code(st->client);
    if (status == 401 && try_refresh(gen)) {
        abs_stream_end(st, true);
        return abs_stream_begin(st, path);
    }
    return status;
}

/* ---------- signing in ---------- */

// A one-off request to any server (for the setup portal, before settings are saved, and for the
// token refresh itself). Returns the HTTP status, or -1 if the server couldn't be reached.
static int raw_request(const char *base, esp_http_client_method_t method, const char *path, const char *bearer,
                       const char *refresh, const char *body, char **out)
{
    *out = NULL;
    char url[256];
    snprintf(url, sizeof(url), "%s%s", base, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .user_agent = USER_AGENT,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    char *hdr = NULL;
    if (bearer && bearer[0]) {
        hdr = heap_caps_malloc(strlen(bearer) + 8, MALLOC_CAP_SPIRAM);
        sprintf(hdr, "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", hdr);
    }
    if (refresh) esp_http_client_set_header(c, "x-refresh-token", refresh);
    // Ask for the refresh token in the response body (otherwise it only goes in a cookie).
    esp_http_client_set_header(c, "x-return-tokens", "true");
    int len = body ? strlen(body) : 0;
    if (body) esp_http_client_set_header(c, "Content-Type", "application/json");
    int status = -1;
    if (esp_http_client_open(c, len) == ESP_OK && (!len || esp_http_client_write(c, body, len) == len) &&
        esp_http_client_fetch_headers(c) >= 0) {
        status = esp_http_client_get_status_code(c);
        size_t cap = 8192, n = 0;
        char *buf = psram_malloc(cap);
        int r;
        while (buf && (r = esp_http_client_read(c, buf + n, cap - n - 1)) > 0) {
            n += r;
            if (cap - n < 1024) {
                char *nb = heap_caps_realloc(buf, cap * 2, MALLOC_CAP_SPIRAM);
                if (!nb) break;
                buf = nb;
                cap *= 2;
            }
        }
        if (buf) buf[n] = 0;
        *out = buf;
    }
    esp_http_client_cleanup(c);
    free(hdr);
    return status;
}

// Tokens from a /login or /auth/refresh response (user.accessToken / user.refreshToken, or the
// pre-2.26 user.token).
static bool parse_tokens(const char *json, char *access, size_t alen, char *refresh, size_t rlen)
{
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    const cJSON *user = cJSON_GetObjectItem(root, "user");
    const char *a = json_str(user, "accessToken");
    if (!a) a = json_str(root, "accessToken");
    if (!a) a = json_str(user, "token");
    const char *r = json_str(user, "refreshToken");
    if (!r) r = json_str(root, "refreshToken");
    if (a) strlcpy(access, a, alen);
    if (refresh) strlcpy(refresh, r ? r : "", rlen);
    cJSON_Delete(root);
    return a != NULL;
}

static bool try_refresh(uint32_t failed_gen)
{
    bool ok = false;
    xSemaphoreTake(s_refresh_lock, portMAX_DELAY);
    if (s_auth_gen != failed_gen) {
        ok = true;  // someone else already renewed it; just retry
    } else if (config_get()->refresh[0]) {
        char *body = NULL;
        char *access = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM), *refresh = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
        int status = raw_request(s_base, HTTP_METHOD_POST, "/auth/refresh", NULL, config_get()->refresh, NULL, &body);
        if (status == 200 && parse_tokens(body, access, 1024, refresh, 1024)) {
            config_set_tokens(access, refresh[0] ? refresh : config_get()->refresh);
            xSemaphoreTake(s_auth_lock, portMAX_DELAY);
            snprintf(s_auth, 1100, "Bearer %s", access);
            s_auth_gen++;
            xSemaphoreGive(s_auth_lock);
            ESP_LOGI(TAG, "access token renewed");
            ok = true;
        } else {
            ESP_LOGW(TAG, "token refresh failed (%d); sign in again from Settings", status);
            if (status == 401 || status == 403) s_signed_out = true;
        }
        free(body);
        free(access);
        free(refresh);
    } else {
        s_signed_out = true;  // an API key that the server no longer accepts
    }
    xSemaphoreGive(s_refresh_lock);
    return ok;
}

bool abs_signed_out(void)
{
    return s_signed_out;
}

abs_auth_result_t abs_login(const char *base, const char *username, const char *password, char *access,
                            size_t alen, char *refresh, size_t rlen)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "username", username);
    cJSON_AddStringToObject(req, "password", password);
    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    char *body = NULL;
    int status = raw_request(base, HTTP_METHOD_POST, "/login", NULL, NULL, json, &body);
    free(json);
    abs_auth_result_t r = status < 0              ? ABS_AUTH_UNREACHABLE
                          : status == 401          ? ABS_AUTH_REJECTED
                          : status != 200          ? ABS_AUTH_ERROR
                          : parse_tokens(body, access, alen, refresh, rlen) ? ABS_AUTH_OK
                                                                            : ABS_AUTH_ERROR;
    ESP_LOGI(TAG, "login to %s as %s: HTTP %d", base, username, status);
    free(body);
    return r;
}

abs_auth_result_t abs_check(const char *base, const char *token)
{
    char *body = NULL;
    int status = raw_request(base, HTTP_METHOD_GET, "/api/me", token, NULL, NULL, &body);
    free(body);
    return status < 0 ? ABS_AUTH_UNREACHABLE
           : status == 401 || status == 403 ? ABS_AUTH_REJECTED
           : status == 200 ? ABS_AUTH_OK
                           : ABS_AUTH_ERROR;
}

int abs_stream_read(abs_stream_t *st, char *buf, int len)
{
    return esp_http_client_read(st->client, buf, len);
}

bool abs_stream_complete(abs_stream_t *st)
{
    return st->client && esp_http_client_is_complete_data_received(st->client);
}

void abs_stream_end(abs_stream_t *st, bool keep_alive)
{
    if (!st->client) return;
    if (keep_alive) {
        // Drain anything left so the connection can carry the next request.
        esp_http_client_flush_response(st->client, NULL);
    } else {
        esp_http_client_cleanup(st->client);
        st->client = NULL;
    }
}

esp_err_t abs_get_cover(const char *item_id, int width, uint8_t **out, size_t *out_len)
{
    char path[128];
    // The server resizes and re-encodes, so this is a small baseline JPEG.
    snprintf(path, sizeof(path), "/api/items/%s/cover?width=%d&format=jpeg", item_id, width);
    char *body = NULL;
    // Only the cover loader task calls this, so it can own a persistent connection.
    static esp_http_client_handle_t conn;
    if (request_len(HTTP_METHOD_GET, path, NULL, &body, out_len, &conn) != 200 || *out_len == 0) {
        free(body);
        return ESP_FAIL;
    }
    *out = (uint8_t *)body;
    return ESP_OK;
}
