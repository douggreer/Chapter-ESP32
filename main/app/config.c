#include "config.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "config";

static app_config_t *s_cfg;  // in PSRAM: the tokens are large
static SemaphoreHandle_t s_lock;

// Flash (NVS) work from tasks with PSRAM stacks is handed to this worker, which has an internal
// stack. It's started at boot, while internal RAM is plentiful: creating a task per save could
// fail later (with the setup portal up, there isn't a 4 KB block left) and lose the save.
#define FLASH_STACK 3072

typedef struct {
    void (*fn)(void *);
    void *arg;
    SemaphoreHandle_t done;
} flash_job_t;

static QueueHandle_t s_jobs;

static void flash_worker(void *unused)
{
    for (;;) {
        flash_job_t *j;
        xQueueReceive(s_jobs, &j, portMAX_DELAY);
        j->fn(j->arg);
        xSemaphoreGive(j->done);
    }
}

void flash_safe(void (*fn)(void *), void *arg)
{
    if (esp_ptr_in_dram((const void *)esp_cpu_get_sp())) {
        fn(arg);
        return;
    }
    StaticSemaphore_t buf;
    flash_job_t job = {fn, arg, xSemaphoreCreateBinaryStatic(&buf)};
    flash_job_t *p = &job;
    xQueueSend(s_jobs, &p, portMAX_DELAY);
    xSemaphoreTake(job.done, portMAX_DELAY);
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) out[0] = 0;
}

static void normalize_root_order(uint8_t order[6])
{
    bool used[6] = {false};
    for (int i = 0; i < 6; ++i) {
        if (order[i] < 6 && !used[order[i]]) {
            used[order[i]] = true;
            continue;
        }
        for (uint8_t candidate = 0; candidate < 6; ++candidate) {
            if (!used[candidate]) {
                order[i] = candidate;
                used[candidate] = true;
                break;
            }
        }
    }
}

void config_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_jobs = xQueueCreate(4, sizeof(flash_job_t *));
    xTaskCreatePinnedToCore(flash_worker, "flash", FLASH_STACK, NULL, 4, NULL, 0);
    s_cfg = heap_caps_calloc(1, sizeof(*s_cfg), MALLOC_CAP_SPIRAM);
    if (!s_cfg) {
        s_cfg = calloc(1, sizeof(*s_cfg));
        if (!s_cfg) abort();
    }
    s_cfg->knob_hold_s = 1;
    s_cfg->acceleration_multiplier = 5;
    s_cfg->acceleration_threshold_ms = 40;
    s_cfg->marquee_delay_ms = 150;
    s_cfg->marquee_speed_px_s = 48;
    s_cfg->marquee_loop_pause_ms = 2000;
    s_cfg->browse_return_s = 10;
    s_cfg->playback_speed_pct = 100;
    s_cfg->display_type = CHAPTER_DISPLAY_SH1122;
    s_cfg->sleep_timer_minutes = 30;
    s_cfg->root_enabled_mask = 0x2f;
    for (int i = 0; i < 6; ++i) s_cfg->root_order[i] = i;
    s_cfg->root_order[4] = 5;
    s_cfg->root_order[5] = 4;

    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "ssid", s_cfg->wifi_ssid, sizeof(s_cfg->wifi_ssid));
        get_str(h, "pass", s_cfg->wifi_pass, sizeof(s_cfg->wifi_pass));
        get_str(h, "server", s_cfg->server, sizeof(s_cfg->server));
        get_str(h, "user", s_cfg->username, sizeof(s_cfg->username));
        get_str(h, "access", s_cfg->access, sizeof(s_cfg->access));
        get_str(h, "refresh", s_cfg->refresh, sizeof(s_cfg->refresh));
        uint8_t v;
        uint16_t wide;
        if (nvs_get_u16(h, "hold_ds", &wide) == ESP_OK) s_cfg->knob_hold_s = wide / 10.0f;
        else if (nvs_get_u8(h, "hold_s", &v) == ESP_OK) s_cfg->knob_hold_s = v;
        if (nvs_get_u8(h, "accel", &v) == ESP_OK) s_cfg->acceleration_multiplier = v;
        if (nvs_get_u8(h, "accel_ms", &v) == ESP_OK) s_cfg->acceleration_threshold_ms = v;
        if (nvs_get_u16(h, "marq_ms", &wide) == ESP_OK) s_cfg->marquee_delay_ms = wide;
        if (nvs_get_u16(h, "marq_px", &wide) == ESP_OK) s_cfg->marquee_speed_px_s = wide;
        if (nvs_get_u16(h, "marq_pause", &wide) == ESP_OK) s_cfg->marquee_loop_pause_ms = wide;
        if (nvs_get_u16(h, "browse_back", &wide) == ESP_OK) s_cfg->browse_return_s = wide;
        if (nvs_get_u8(h, "speed_pct", &v) == ESP_OK) s_cfg->playback_speed_pct = v;
        if (nvs_get_u8(h, "sort_mode", &v) == ESP_OK) s_cfg->library_sort_mode = v;
        if (nvs_get_u8(h, "sleep_min", &v) == ESP_OK) s_cfg->sleep_timer_minutes = v;
        if (nvs_get_u8(h, "sleep_on", &v) == ESP_OK) s_cfg->sleep_timer_enabled = v != 0;
        if (nvs_get_u8(h, "display", &v) == ESP_OK) s_cfg->display_type = v;
        if (nvs_get_u8(h, "root_mask", &v) == ESP_OK) s_cfg->root_enabled_mask = v;
        for (int i = 0; i < 6; ++i) {
            char key[8];
            snprintf(key, sizeof(key), "root%d", i);
            if (nvs_get_u8(h, key, &v) == ESP_OK) s_cfg->root_order[i] = v;
        }
        if (nvs_get_u8(h, "resume", &v) == ESP_OK) s_cfg->resume_last_at_startup = v != 0;
        nvs_close(h);
    }
    normalize_root_order(s_cfg->root_order);
    if (s_cfg->display_type > CHAPTER_DISPLAY_SSD1306) s_cfg->display_type = CHAPTER_DISPLAY_SH1122;

    ESP_LOGI(TAG, "wifi '%s', server %s, account '%s', %s", s_cfg->wifi_ssid, s_cfg->server,
             s_cfg->username[0] ? s_cfg->username : "(API key or unknown)",
             s_cfg->refresh[0] ? "signed in" : (s_cfg->access[0] ? "API key" : "no credentials"));
}

const app_config_t *config_get(void)
{
    return s_cfg;
}

typedef struct {
    esp_err_t result;
} config_write_result_t;

static void write_all(void *arg)
{
    config_write_result_t *result = arg;
    nvs_handle_t h;
    esp_err_t err = nvs_open("cfg", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "can't save settings: %s", esp_err_to_name(err));
        result->result = err;
        return;
    }
    const esp_err_t errs[] = {
        nvs_set_str(h, "ssid", s_cfg->wifi_ssid),
        nvs_set_str(h, "pass", s_cfg->wifi_pass),
        nvs_set_str(h, "server", s_cfg->server),
        nvs_set_str(h, "user", s_cfg->username),
        nvs_set_str(h, "access", s_cfg->access),
        nvs_set_str(h, "refresh", s_cfg->refresh),
        nvs_set_u8(h, "hold_s", (uint8_t)(s_cfg->knob_hold_s + 0.5f)),
        nvs_set_u8(h, "accel", s_cfg->acceleration_multiplier),
        nvs_set_u8(h, "accel_ms", s_cfg->acceleration_threshold_ms),
        nvs_set_u16(h, "marq_ms", s_cfg->marquee_delay_ms),
        nvs_set_u16(h, "marq_px", s_cfg->marquee_speed_px_s),
        nvs_set_u16(h, "marq_pause", s_cfg->marquee_loop_pause_ms),
        nvs_set_u16(h, "browse_back", s_cfg->browse_return_s),
        nvs_set_u16(h, "hold_ds", (uint16_t)(s_cfg->knob_hold_s * 10.0f + 0.5f)),
        nvs_set_u8(h, "speed_pct", s_cfg->playback_speed_pct),
        nvs_set_u8(h, "sort_mode", s_cfg->library_sort_mode),
        nvs_set_u8(h, "sleep_min", s_cfg->sleep_timer_minutes),
        nvs_set_u8(h, "sleep_on", s_cfg->sleep_timer_enabled),
        nvs_set_u8(h, "display", s_cfg->display_type),
        nvs_set_u8(h, "root_mask", s_cfg->root_enabled_mask),
        nvs_set_u8(h, "resume", s_cfg->resume_last_at_startup),
    };
    for (int i = 0; i < sizeof(errs) / sizeof(errs[0]); i++) {
        if (errs[i] != ESP_OK) {
            err = errs[i];
            ESP_LOGE(TAG, "saving settings failed (step %d): %s", i, esp_err_to_name(err));
            goto done;
        }
    }
    for (int i = 0; i < 6; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "root%d", i);
        err = nvs_set_u8(h, key, s_cfg->root_order[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "saving root menu order failed: %s", esp_err_to_name(err));
            goto done;
        }
    }
    err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "committing settings failed: %s", esp_err_to_name(err));
done:
    nvs_close(h);
    result->result = err;
}

bool config_save(const app_config_t *cfg)
{
    app_config_t *before = heap_caps_malloc(sizeof(*before), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!before) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *before = *s_cfg;
    if (cfg != s_cfg) *s_cfg = *cfg;
    if (s_cfg->knob_hold_s < 0.5f) s_cfg->knob_hold_s = 0.5f;
    if (s_cfg->knob_hold_s > 5.0f) s_cfg->knob_hold_s = 5.0f;
    s_cfg->knob_hold_s = ((int)(s_cfg->knob_hold_s * 10.0f + 0.5f)) / 10.0f;
    if (s_cfg->acceleration_multiplier != 0 && s_cfg->acceleration_multiplier < 2) s_cfg->acceleration_multiplier = 2;
    if (s_cfg->acceleration_multiplier > 10) s_cfg->acceleration_multiplier = 10;
    if (s_cfg->acceleration_threshold_ms < 20) s_cfg->acceleration_threshold_ms = 20;
    if (s_cfg->acceleration_threshold_ms > 250) s_cfg->acceleration_threshold_ms = 250;
    if (s_cfg->marquee_delay_ms < 0) s_cfg->marquee_delay_ms = 0;
    if (s_cfg->marquee_delay_ms > 2000) s_cfg->marquee_delay_ms = 2000;
    if (s_cfg->marquee_speed_px_s < 20) s_cfg->marquee_speed_px_s = 20;
    if (s_cfg->marquee_speed_px_s > 200) s_cfg->marquee_speed_px_s = 200;
    if (s_cfg->marquee_loop_pause_ms < 0) s_cfg->marquee_loop_pause_ms = 0;
    if (s_cfg->marquee_loop_pause_ms > 10000) s_cfg->marquee_loop_pause_ms = 10000;
    if (s_cfg->browse_return_s < 0) s_cfg->browse_return_s = 0;
    if (s_cfg->browse_return_s > 120) s_cfg->browse_return_s = 120;
    if (s_cfg->playback_speed_pct != 125 && s_cfg->playback_speed_pct != 150 &&
        s_cfg->playback_speed_pct != 200) s_cfg->playback_speed_pct = 100;
    if (s_cfg->library_sort_mode > 1) s_cfg->library_sort_mode = 0;
    if (s_cfg->sleep_timer_minutes < 1) s_cfg->sleep_timer_minutes = 1;
    if (s_cfg->sleep_timer_minutes > 240) s_cfg->sleep_timer_minutes = 240;
    if (s_cfg->display_type > CHAPTER_DISPLAY_SSD1306) s_cfg->display_type = CHAPTER_DISPLAY_SH1122;
    s_cfg->root_enabled_mask &= 0x2f;
    if (!s_cfg->root_enabled_mask) s_cfg->root_enabled_mask = 1;
    normalize_root_order(s_cfg->root_order);
    config_write_result_t result = {.result = ESP_FAIL};
    flash_safe(write_all, &result);
    if (result.result != ESP_OK) *s_cfg = *before;
    xSemaphoreGive(s_lock);
    free(before);
    return result.result == ESP_OK;
}

void config_set_tokens(const char *access, const char *refresh)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cfg->access, access, sizeof(s_cfg->access));
    if (refresh) strlcpy(s_cfg->refresh, refresh, sizeof(s_cfg->refresh));
    config_write_result_t result = {.result = ESP_FAIL};
    flash_safe(write_all, &result);
    if (result.result != ESP_OK) ESP_LOGE(TAG, "could not persist refreshed Audiobookshelf tokens");
    xSemaphoreGive(s_lock);
}

bool config_complete(void)
{
    return s_cfg->wifi_ssid[0] && s_cfg->server[0] && s_cfg->access[0];
}
