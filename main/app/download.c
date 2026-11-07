#include "download.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage.h"
#include "wifi.h"

static const char *TAG = "download";

#define DL_ROOT          STORAGE_ROOT "/dl"
#define SEGMENT_SECONDS  6.0
#define MAX_DOWNLOADS    64
#define SYNC_EVERY       10  // segments between fsyncs (index entries are written only after a sync)
#define SEGMENT_RETRIES  40
#define MAX_FAILURES     3   // whole-run failures before a download is marked as errored

typedef struct {
    char id[40];
    dl_state_t state;
    int done;   // completed segments
    int total;  // 0 until the session has been started once
    int failures;
} entry_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static entry_t s_entries[MAX_DOWNLOADS];
static int s_count;
static volatile uint32_t s_generation;
static volatile bool s_cancel;  // cancels the active download
static char s_running[40];      // id of the download being run, if any
static abs_stream_t *s_stream;

/* ---------- paths and small files ---------- */

static void book_path(const char *id, const char *file, char *out, size_t len)
{
    snprintf(out, len, DL_ROOT "/%s%s%s", id, file ? "/" : "", file ? file : "");
}

static int index_entries(const char *id)
{
    char p[128];
    book_path(id, "index.bin", p, sizeof(p));
    struct stat st;
    return stat(p, &st) == 0 ? st.st_size / 4 : 0;
}

static bool file_exists(const char *id, const char *file)
{
    char p[128];
    book_path(id, file, p, sizeof(p));
    struct stat st;
    return stat(p, &st) == 0;
}

/* ---------- registry (under s_lock) ---------- */

static entry_t *find(const char *id)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].id, id) == 0) return &s_entries[i];
    }
    return NULL;
}

static void bump(void)
{
    s_generation++;
}

static void scan(void)
{
    DIR *d = opendir(DL_ROOT);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_count < MAX_DOWNLOADS) {
        if (e->d_type != DT_DIR || strlen(e->d_name) >= sizeof(s_entries[0].id)) continue;
        if (!file_exists(e->d_name, "meta.json") && !file_exists(e->d_name, "queued")) continue;
        entry_t *en = &s_entries[s_count++];
        memset(en, 0, sizeof(*en));
        strlcpy(en->id, e->d_name, sizeof(en->id));
        en->done = index_entries(en->id);
        if (file_exists(en->id, "complete")) {
            en->state = DL_DONE;
            en->total = en->done;
        } else {
            en->state = DL_QUEUED;  // resume
        }
        ESP_LOGI(TAG, "%.8s: %s, %d segments", en->id, en->state == DL_DONE ? "complete" : "resuming", en->done);
    }
    closedir(d);
}

/* ---------- downloading ---------- */

static esp_err_t write_meta(const char *id, const abs_session_t *s, int total)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "duration", s->duration);
    cJSON_AddNumberToObject(root, "segments", total);
    cJSON *chapters = cJSON_AddArrayToObject(root, "chapters");
    for (int i = 0; i < s->chapter_count; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "title", s->chapters[i].title);
        cJSON_AddNumberToObject(c, "start", s->chapters[i].start);
        cJSON_AddNumberToObject(c, "end", s->chapters[i].end);
        cJSON_AddItemToArray(chapters, c);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    char p[128];
    book_path(id, "meta.json", p, sizeof(p));
    esp_err_t err = json ? storage_write_file(p, json, strlen(json)) : ESP_FAIL;
    free(json);
    return err;
}

// Appends one segment to `audio`. Retries (404/500 while the server transcodes or re-seeks).
static bool fetch_segment(const char *base, int seg, FILE *audio, uint8_t *buf, int buf_len)
{
    char path[128];
    snprintf(path, sizeof(path), "%soutput-%d.ts", base, seg);
    const long start = ftell(audio);
    for (int attempt = 0; attempt < SEGMENT_RETRIES && !s_cancel; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(attempt < 3 ? 500 : 1000));
        int status = abs_stream_begin(s_stream, path);
        if (status != 200) {
            abs_stream_end(s_stream, status > 0);
            continue;
        }
        fseek(audio, start, SEEK_SET);  // drop anything from a failed attempt
        bool ok = true;
        for (;;) {
            int n = abs_stream_read(s_stream, (char *)buf, buf_len);
            if (n < 0 || s_cancel) {
                ok = false;
                break;
            }
            if (n == 0) {
                ok = abs_stream_complete(s_stream);
                break;
            }
            if (fwrite(buf, 1, n, audio) != (size_t)n) {
                ESP_LOGE(TAG, "download write failed (onboard flash full?)");
                ok = false;
                s_cancel = true;
                break;
            }
        }
        abs_stream_end(s_stream, ok);
        if (ok) return true;
    }
    return false;
}

// Makes the audio durable, then records the segments it contains in the index.
static bool commit(FILE *audio, FILE *index, uint32_t *ends, int *pending)
{
    if (*pending == 0) return true;
    if (fflush(audio) != 0 || fsync(fileno(audio)) != 0) return false;
    if (fwrite(ends, 4, *pending, index) != (size_t)*pending) return false;
    if (fflush(index) != 0 || fsync(fileno(index)) != 0) return false;
    *pending = 0;
    return true;
}

// Downloads (or resumes) one book. Returns true when complete.
static bool run(entry_t *en_snapshot)
{
    char id[40];
    strlcpy(id, en_snapshot->id, sizeof(id));
    abs_session_t session;
    if (abs_start_session(id, NULL, true, &session) != ESP_OK) return false;

    const int total = (int)ceil(session.duration / SEGMENT_SECONDS);
    if (!file_exists(id, "meta.json")) write_meta(id, &session, total);

    char base[96];
    snprintf(base, sizeof(base), "%.*s", (int)(strrchr(session.hls_path, '/') - session.hls_path + 1), session.hls_path);

    char p_audio[128], p_index[128];
    book_path(id, "audio.ts", p_audio, sizeof(p_audio));
    book_path(id, "index.bin", p_index, sizeof(p_index));

    // Resume point: the end of the last committed segment. Anything after it is discarded.
    int done = index_entries(id);
    uint32_t resume = 0;
    FILE *index = fopen(p_index, "a+b");
    if (index && done > 0) {
        fseek(index, (done - 1) * 4, SEEK_SET);
        fread(&resume, 4, 1, index);
        fseek(index, 0, SEEK_END);
    }
    FILE *audio = fopen(p_audio, done ? "r+b" : "wb");
    if (!audio || !index) {
        ESP_LOGE(TAG, "can't open files for %.8s", id);
        if (audio) fclose(audio);
        if (index) fclose(index);
        abs_close_session(session.id, 0, 0);
        abs_free_session(&session);
        return false;
    }
    truncate(p_audio, resume);
    fseek(audio, resume, SEEK_SET);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *en = find(id);
    if (en && en->state == DL_QUEUED) {
        en->state = DL_ACTIVE;
        en->total = total;
        en->done = done;
    }
    bump();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%.8s: downloading from segment %d of %d", id, done, total);

    // Keep the streamed-segment scratch buffer in DMA-capable internal RAM for IDF driver compatibility.
    const int buf_len = 4096;
    uint8_t *buf = heap_caps_malloc(buf_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint32_t ends[SYNC_EVERY];
    int pending = 0;
    bool ok = buf != NULL;
    for (int seg = done; ok && seg < total && !s_cancel; seg++) {
        if (!fetch_segment(base, seg, audio, buf, buf_len)) {
            ok = false;
            break;
        }
        ends[pending++] = (uint32_t)ftell(audio);
        if (pending == SYNC_EVERY || seg == total - 1) {
            if (!commit(audio, index, ends, &pending)) {
                ok = false;
                break;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if ((en = find(id))) en->done = seg + 1;
            bump();
            xSemaphoreGive(s_lock);
        }
    }
    commit(audio, index, ends, &pending);
    free(buf);
    fclose(audio);
    fclose(index);
    // An empty close body leaves the user's progress untouched.
    abs_close_session(session.id, 0, 0);
    abs_free_session(&session);

    bool complete = ok && !s_cancel && index_entries(id) >= total;
    if (complete) {
        char p[128];
        book_path(id, "complete", p, sizeof(p));
        storage_write_file(p, "1", 1);
        ESP_LOGI(TAG, "%.8s: complete", id);
    }
    return complete;
}

// Deletes the files of every entry marked for removal (not the one being downloaded).
static void process_removals(void)
{
    for (;;) {
        char id[40] = "";
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < s_count; i++) {
            if (s_entries[i].state == DL_REMOVING) {
                strlcpy(id, s_entries[i].id, sizeof(id));
                break;
            }
        }
        xSemaphoreGive(s_lock);
        if (!id[0]) return;

        vTaskDelay(pdMS_TO_TICKS(500));  // let the player close the file if it was playing it
        char dir[128];
        book_path(id, NULL, dir, sizeof(dir));
        storage_remove_tree(dir);
        ESP_LOGI(TAG, "%.8s: removed", id);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        entry_t *en = find(id);
        if (en) *en = s_entries[--s_count];  // swap-remove
        bump();
        xSemaphoreGive(s_lock);
    }
}

static void download_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
        for (;;) {
            process_removals();
            if (!wifi_is_connected()) break;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            entry_t *next = NULL;
            for (int i = 0; i < s_count && !next; i++) {
                if (s_entries[i].state == DL_QUEUED) next = &s_entries[i];
            }
            entry_t snapshot = next ? *next : (entry_t){0};
            strlcpy(s_running, snapshot.id, sizeof(s_running));
            s_cancel = false;
            xSemaphoreGive(s_lock);
            if (!next) break;

            bool complete = run(&snapshot);

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_running[0] = 0;
            entry_t *en = find(snapshot.id);
            bool failed = false;
            if (en && en->state != DL_REMOVING) {
                if (complete) {
                    en->state = DL_DONE;
                    en->done = en->total;
                } else if (++en->failures >= MAX_FAILURES) {
                    en->state = DL_ERROR;
                } else {
                    en->state = DL_QUEUED;
                    failed = true;
                }
            }
            bump();
            xSemaphoreGive(s_lock);
            if (failed) break;  // back off before retrying
        }
    }
}

/* ---------- public ---------- */

void download_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!storage_ready()) return;
    storage_mkdirs(DL_ROOT);
    scan();
    s_stream = abs_stream_create();
    // Stack in PSRAM: this task does network and SD I/O but never touches the internal flash.
    xTaskCreatePinnedToCoreWithCaps(download_task, "download", 8192, NULL, 2, &s_task, 0, MALLOC_CAP_SPIRAM);
}

esp_err_t download_start(const abs_book_t *book)
{
    if (!storage_ready() || !s_task) return ESP_ERR_INVALID_STATE;
    char dir[128], marker[128];
    book_path(book->id, NULL, dir, sizeof(dir));
    book_path(book->id, "queued", marker, sizeof(marker));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *en = find(book->id);
    if (!en && s_count < MAX_DOWNLOADS) {
        en = &s_entries[s_count++];
        memset(en, 0, sizeof(*en));
        strlcpy(en->id, book->id, sizeof(en->id));
    }
    if (en && (en->state == DL_NONE || en->state == DL_ERROR)) {
        en->state = DL_QUEUED;
        en->failures = 0;
    }
    bump();
    xSemaphoreGive(s_lock);
    if (!en) return ESP_ERR_NO_MEM;

    // A marker so a queued-but-not-started download survives a reboot.
    storage_mkdirs(dir);
    storage_write_file(marker, "1", 1);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void download_remove(const char *item_id)
{
    if (!storage_ready() || !s_task) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *en = find(item_id);
    if (en) {
        en->state = DL_REMOVING;
        if (strcmp(s_running, item_id) == 0) s_cancel = true;
    }
    bump();
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

dl_state_t download_state(const char *item_id, int *percent)
{
    dl_state_t st = DL_NONE;
    int pct = 0;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        entry_t *en = find(item_id);
        if (en) {
            st = en->state;
            pct = en->total ? (100 * en->done / en->total) : 0;
        }
        xSemaphoreGive(s_lock);
    }
    if (percent) *percent = pct;
    return st;
}

bool download_busy(void)
{
    if (!s_lock) return false;
    bool busy = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && !busy; i++) {
        busy = s_entries[i].state == DL_QUEUED || s_entries[i].state == DL_ACTIVE || s_entries[i].state == DL_REMOVING;
    }
    xSemaphoreGive(s_lock);
    return busy;
}

uint32_t download_generation(void)
{
    return s_generation;
}

esp_err_t download_load_meta(const char *item_id, abs_session_t *out)
{
    memset(out, 0, sizeof(*out));
    char p[128];
    book_path(item_id, "meta.json", p, sizeof(p));
    char *json = storage_read_file(p, NULL);
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    free(json);
    if (!root) return ESP_FAIL;
    out->duration = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "duration"));
    const cJSON *chapters = cJSON_GetObjectItem(root, "chapters");
    int n = cJSON_GetArraySize(chapters);
    if (n > 0) {
        out->chapters = heap_caps_calloc(n, sizeof(abs_chapter_t), MALLOC_CAP_SPIRAM);
        const cJSON *c;
        cJSON_ArrayForEach(c, chapters) {
            if (!out->chapters) break;
            abs_chapter_t *ch = &out->chapters[out->chapter_count++];
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(c, "title"));
            ch->title = strdup(t ? t : "");
            ch->start = cJSON_GetNumberValue(cJSON_GetObjectItem(c, "start"));
            ch->end = cJSON_GetNumberValue(cJSON_GetObjectItem(c, "end"));
        }
    }
    cJSON_Delete(root);
    return out->duration > 0 ? ESP_OK : ESP_FAIL;
}

void download_audio_path(const char *item_id, char *path, size_t len)
{
    book_path(item_id, "audio.ts", path, len);
}

esp_err_t download_segment_offset(const char *item_id, int seg, uint32_t *offset)
{
    *offset = 0;
    if (seg <= 0) return ESP_OK;
    char p[128];
    book_path(item_id, "index.bin", p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) return ESP_FAIL;
    bool ok = fseek(f, (seg - 1) * 4, SEEK_SET) == 0 && fread(offset, 4, 1, f) == 1;
    fclose(f);
    return ok ? ESP_OK : ESP_FAIL;
}

bool download_get_progress(const char *item_id, double *position, bool *pending)
{
    char p[128];
    book_path(item_id, "progress.json", p, sizeof(p));
    char *json = storage_read_file(p, NULL);
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    free(json);
    if (!root) return false;
    *position = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "position"));
    *pending = cJSON_IsTrue(cJSON_GetObjectItem(root, "pending"));
    cJSON_Delete(root);
    return true;
}

void download_set_progress(const char *item_id, double position, bool pending)
{
    char p[128], json[64];
    book_path(item_id, "progress.json", p, sizeof(p));
    snprintf(json, sizeof(json), "{\"position\":%.2f,\"pending\":%s}", position, pending ? "true" : "false");
    storage_write_file(p, json, strlen(json));
}

void download_sync_progress(abs_book_t *books, int count, bool online)
{
    if (!storage_ready()) return;
    for (int i = 0; i < count; i++) {
        double pos;
        bool pending;
        if (download_state(books[i].id, NULL) != DL_DONE || !download_get_progress(books[i].id, &pos, &pending) ||
            !pending) {
            continue;
        }
        // Listening that happened offline wins over what the server last heard.
        books[i].current_time = pos;
        books[i].progress = books[i].duration > 0 ? pos / books[i].duration : 0;
        if (online && abs_patch_progress(books[i].id, pos, books[i].duration, false) == ESP_OK) {
            download_set_progress(books[i].id, pos, false);
            ESP_LOGI(TAG, "%.8s: pushed offline position %.0f s", books[i].id, pos);
        }
    }
}
