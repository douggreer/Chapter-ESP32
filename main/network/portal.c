#include "portal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "abs_api.h"
#include "catalog.h"
#include "cJSON.h"
#include "config.h"
#include "device_history.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "portal_page.h"
#include "player.h"
#include "power.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "portal";

#define AP_IP "192.168.4.1"
#define PORTAL_BOOKS_PAGE_SIZE 250

static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task, s_apply_task;
static volatile bool s_active, s_setup_active, s_dns_stop;
static char s_ssid[33], s_pass[16];

// Progress of the last save, shown on the page and the device screen.
static SemaphoreHandle_t s_lock;
static char s_message[160] = "Waiting for your phone or laptop";
static bool s_busy;
static cJSON *s_pending;  // the submitted settings, handed to the apply task
static volatile bool s_bring_up;  // the setup task should scan, then start the access point

// Nearby networks, scanned once before the setup network starts: scanning takes the radio off
// the setup network's channel for seconds, which drops the phone that asked for it.
#define MAX_SCAN 20
static wifi_ap_record_t *s_scan;
static volatile int s_scan_count = -1;  // -1 until the scan finishes

static void set_status(bool busy, const char *fmt, ...)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_message, sizeof(s_message), fmt, ap);
    va_end(ap);
    s_busy = busy;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s", s_message);
}

/* ---------- DNS: every name resolves to us, so phones open the setup page ---------- */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    struct timeval tv = {.tv_sec = 1};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    while (!s_dns_stop) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;
        // Turn the query into a response with one A record pointing at the setup address.
        buf[2] = 0x81;  // response, recursion desired
        buf[3] = 0x80;  // recursion available, no error
        buf[6] = 0;     // one answer
        buf[7] = 1;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
        memcpy(buf + n, answer, sizeof(answer));
        sendto(sock, buf, n + sizeof(answer), 0, (struct sockaddr *)&from, flen);
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* ---------- HTTP ---------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not encode response");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t on_page(httpd_req_t *req)
{
    power_note_activity();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PORTAL_PAGE, sizeof(PORTAL_PAGE) - 1);
}

// Anything else (including the OS "is there internet?" probes) goes to the setup page.
static esp_err_t on_other(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_setup_active ? "http://" AP_IP "/" : "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t on_config(httpd_req_t *req)
{
    const app_config_t *c = config_get();
    power_config_t pc;
    power_get_config(&pc);
    // Never send stored passwords or tokens back to the browser.
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", c->wifi_ssid);
    cJSON_AddStringToObject(o, "server", c->server);
    cJSON_AddStringToObject(o, "username", c->username);
    cJSON_AddStringToObject(o, "auth", (c->access[0] && !c->refresh[0] && !c->username[0]) ? "apikey" : "password");
    cJSON_AddBoolToObject(o, "signed_in", c->access[0] != 0);
    cJSON_AddNumberToObject(o, "brightness", pc.brightness);
    cJSON_AddNumberToObject(o, "idle_timeout_min", pc.idle_timeout_minutes);
    cJSON_AddNumberToObject(o, "screen_saver_mode", pc.screen_saver_mode);
    cJSON_AddNumberToObject(o, "screen_saver_dim_percent", pc.screen_saver_dim_percent);
    cJSON_AddBoolToObject(o, "power_save_enabled", pc.power_save_enabled);
    cJSON_AddNumberToObject(o, "knob_hold_s", c->knob_hold_s);
    cJSON_AddNumberToObject(o, "acceleration_multiplier", c->acceleration_multiplier);
    cJSON_AddNumberToObject(o, "acceleration_threshold_ms", c->acceleration_threshold_ms);
    cJSON_AddNumberToObject(o, "marquee_delay_ms", c->marquee_delay_ms);
    cJSON_AddNumberToObject(o, "marquee_speed_px_s", c->marquee_speed_px_s);
    cJSON_AddNumberToObject(o, "marquee_loop_pause_ms", c->marquee_loop_pause_ms);
    cJSON_AddNumberToObject(o, "browse_return_s", c->browse_return_s);
    cJSON_AddNumberToObject(o, "playback_speed_pct", c->playback_speed_pct);
    cJSON_AddNumberToObject(o, "library_sort_mode", c->library_sort_mode);
    cJSON_AddNumberToObject(o, "series_sort_mode", c->series_sort_mode);
    cJSON_AddNumberToObject(o, "author_sort_mode", c->author_sort_mode);
    cJSON_AddBoolToObject(o, "sleep_timer_enabled", c->sleep_timer_enabled);
    cJSON_AddNumberToObject(o, "sleep_timer_minutes", c->sleep_timer_minutes);
    cJSON_AddNumberToObject(o, "display_type", c->display_type);
    cJSON_AddBoolToObject(o, "wifi_connected", wifi_is_connected());
    cJSON_AddBoolToObject(o, "signed_out", abs_signed_out());
    cJSON_AddStringToObject(o, "library", abs_library_name() ? abs_library_name() : "");
    uint32_t library_revision = 0;
    ui_get_source(&library_revision, NULL);
    cJSON_AddNumberToObject(o, "library_revision", library_revision);
    cJSON_AddNumberToObject(o, "root_enabled_mask", c->root_enabled_mask);
    cJSON_AddBoolToObject(o, "resume_last_at_startup", c->resume_last_at_startup);
    cJSON *root_order = cJSON_CreateArray();
    for (int i = 0; i < 6; ++i) cJSON_AddItemToArray(root_order, cJSON_CreateNumber(c->root_order[i]));
    cJSON_AddItemToObject(o, "root_order", root_order);
    return send_json(req, o);
}

static esp_err_t on_scan(httpd_req_t *req)
{
    const wifi_ap_record_t *aps = s_scan;
    const int n = s_scan_count;
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(o, "secure", aps[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, arr);
}

static esp_err_t on_status(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddStringToObject(o, "message", s_message);
    cJSON_AddBoolToObject(o, "busy", s_busy);
    xSemaphoreGive(s_lock);
    return send_json(req, o);
}

static esp_err_t on_player(httpd_req_t *req)
{
    player_status_t state;
    player_get_status(&state);
    static const char *names[] = {"idle", "loading", "buffering", "playing", "paused", "finished", "error"};
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", state.state >= PLAYER_IDLE && state.state <= PLAYER_ERROR
                                           ? names[state.state] : "unknown");
    cJSON_AddStringToObject(o, "id", state.item_id);
    cJSON_AddStringToObject(o, "title", state.title);
    cJSON_AddStringToObject(o, "author", state.author);
    cJSON_AddStringToObject(o, "chapter", state.chapter);
    cJSON_AddNumberToObject(o, "position", state.position);
    cJSON_AddNumberToObject(o, "duration", state.duration);
    cJSON_AddNumberToObject(o, "volume", state.volume);
    return send_json(req, o);
}

static esp_err_t on_books(httpd_req_t *req)
{
    size_t offset = 0;
    char query[64], value[24];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "offset", value, sizeof(value)) == ESP_OK) {
        offset = strtoul(value, NULL, 10);
    }
    catalog_web_item_t *books = heap_caps_calloc(PORTAL_BOOKS_PAGE_SIZE, sizeof(*books),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!books) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    int count = catalog_copy_web_items(offset, books, PORTAL_BOOKS_PAGE_SIZE);
    cJSON *array = cJSON_CreateArray();
    for (int i = 0; books && i < count; ++i) {
        cJSON *book = cJSON_CreateObject();
        cJSON_AddStringToObject(book, "id", books[i].id);
        cJSON_AddStringToObject(book, "title", books[i].title);
        cJSON_AddStringToObject(book, "sort_title", books[i].sort_title);
        cJSON_AddStringToObject(book, "author", books[i].author);
        cJSON_AddStringToObject(book, "series", books[i].series);
        cJSON_AddStringToObject(book, "series_sequence", books[i].series_sequence);
        cJSON_AddNumberToObject(book, "series_total", books[i].series_total);
        cJSON_AddNumberToObject(book, "duration", books[i].duration);
        cJSON_AddNumberToObject(book, "current_time", books[i].current_time);
        cJSON_AddNumberToObject(book, "last_update", books[i].last_update);
        cJSON_AddNumberToObject(book, "progress", books[i].progress);
        cJSON_AddBoolToObject(book, "finished", books[i].finished);
        cJSON *authors = cJSON_CreateArray();
        for (int a = 0; a < books[i].author_count; ++a) {
            cJSON_AddItemToArray(authors, cJSON_CreateString(books[i].authors[a]));
        }
        cJSON_AddItemToObject(book, "authors", authors);
        cJSON_AddItemToArray(array, book);
    }
    free(books);
    return send_json(req, array);
}

static esp_err_t on_recent(httpd_req_t *req)
{
    device_history_entry_t entries[DEVICE_HISTORY_LIMIT];
    const int count = device_history_copy(entries, DEVICE_HISTORY_LIMIT);
    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < count; ++i) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", entries[i].id);
        cJSON_AddStringToObject(entry, "title", entries[i].title);
        cJSON_AddStringToObject(entry, "author", entries[i].author);
        cJSON_AddItemToArray(array, entry);
    }
    return send_json(req, array);
}

static cJSON *read_json_body(httpd_req_t *req, int limit)
{
    if (req->content_len <= 0 || req->content_len > limit) return NULL;
    char *body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int got = 0;
    while (body && got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) break;
        got += n;
    }
    cJSON *root = NULL;
    if (body && got == req->content_len) {
        body[got] = 0;
        root = cJSON_Parse(body);
    }
    free(body);
    return root;
}

static esp_err_t on_play(httpd_req_t *req)
{
    cJSON *body = read_json_body(req, 256);
    const cJSON *id = body ? cJSON_GetObjectItemCaseSensitive(body, "id") : NULL;
    const bool accepted = cJSON_IsString(id) && catalog_play_book(id->valuestring);
    cJSON_Delete(body);
    if (!accepted) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Book not found in the loaded library");
    ui_show_player();
    power_note_activity();
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "accepted", true);
    return send_json(req, result);
}

static esp_err_t on_control(httpd_req_t *req)
{
    cJSON *body = read_json_body(req, 256);
    const cJSON *action = body ? cJSON_GetObjectItemCaseSensitive(body, "action") : NULL;
    if (!cJSON_IsString(action)) {
        cJSON_Delete(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing control action");
    }
    const char *name = action->valuestring;
    if (strcmp(name, "toggle") == 0) player_toggle();
    else if (strcmp(name, "stop") == 0) player_stop();
    else if (strcmp(name, "previous") == 0) player_chapter_step(-1);
    else if (strcmp(name, "next") == 0) player_chapter_step(1);
    else if (strcmp(name, "back") == 0) player_seek_relative(-30);
    else if (strcmp(name, "forward") == 0) player_seek_relative(30);
    else if (strcmp(name, "refresh") == 0) ui_request_refresh();
    else if (strcmp(name, "volume") == 0) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(body, "value");
        if (!cJSON_IsNumber(value)) {
            cJSON_Delete(body);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Volume must be a number");
        }
        player_set_volume(value->valueint);
    } else {
        cJSON_Delete(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown control action");
    }
    const bool show_player = strcmp(name, "refresh") != 0 && strcmp(name, "volume") != 0;
    cJSON_Delete(body);
    if (show_player) ui_show_player();
    power_note_activity();
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "accepted", true);
    return send_json(req, result);
}

static esp_err_t on_save(httpd_req_t *req)
{
    if (!s_apply_task) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "accepted", false);
        cJSON_AddStringToObject(o, "message", "Settings service is unavailable. Restart the player and try again.");
        return send_json(req, o);
    }
    if (req->content_len <= 0 || req->content_len > 4096) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    char *body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    int got = 0;
    while (body && got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) break;
        got += r;
    }
    cJSON *root = NULL;
    if (body && got == req->content_len) {
        body[got] = 0;
        root = cJSON_Parse(body);
    }
    free(body);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    power_note_activity();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = s_busy;
    if (!busy) {
        cJSON_Delete(s_pending);
        s_pending = root;
        s_busy = true;
        strlcpy(s_message, "Saving...", sizeof(s_message));
    }
    xSemaphoreGive(s_lock);
    if (busy) {
        cJSON_Delete(root);
    } else {
        xTaskNotifyGive(s_apply_task);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "accepted", !busy);
    return send_json(req, o);
}

/* ---------- applying a save ---------- */

static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static int num(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static float decimal(const cJSON *o, const char *key, float def)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void apply(const cJSON *req)
{
    app_config_t *c = heap_caps_malloc(sizeof(*c), MALLOC_CAP_SPIRAM);
    if (!c) {
        set_status(false, "Not enough memory to save settings.");
        return;
    }
    const app_config_t *previous_config = config_get();
    *c = *previous_config;

    // Wi-Fi: blank password means keep the saved one (if the network didn't change).
    const char *ssid = str(req, "ssid"), *wpass = str(req, "wifi_password");
    if (!ssid[0]) {
        set_status(false, "Choose a Wi-Fi network.");
        goto out;
    }
    const bool same_net = strcmp(ssid, c->wifi_ssid) == 0;
    const char *use_pass = (wpass[0] || !same_net) ? wpass : c->wifi_pass;
    // Already on that network with those details: no need to test (and hop channels).
    const bool unchanged = same_net && strcmp(use_pass, c->wifi_pass) == 0 && wifi_is_connected();
    if (!unchanged) set_status(true, "Connecting to %s...", ssid);
    if (!unchanged && !wifi_try_connect(ssid, use_pass, 20000)) {
        set_status(false, "Couldn't join %s. Check the network and password.", ssid);
        goto out;
    }
    strlcpy(c->wifi_ssid, ssid, sizeof(c->wifi_ssid));
    strlcpy(c->wifi_pass, use_pass, sizeof(c->wifi_pass));

    // Server: trimmed, no trailing slash, https:// assumed if no scheme given.
    char server[128];
    const char *srv = str(req, "server");
    if (!srv[0]) {
        set_status(false, "Enter your Audiobookshelf server address.");
        goto out;
    }
    snprintf(server, sizeof(server), "%s%s", strstr(srv, "://") ? "" : "https://", srv);
    size_t n = strlen(server);
    while (n && server[n - 1] == '/') server[--n] = 0;

    // Sign in: username/password, an API key, or keep the current credential.
    abs_auth_result_t r;
    const char *mode = str(req, "auth"), *user = str(req, "username"), *pass = str(req, "password");
    const char *key = str(req, "api_key");
    if (strcmp(mode, "apikey") == 0) {
        if (key[0]) {
            set_status(true, "Checking the API key...");
            r = abs_check(server, key);
            if (r == ABS_AUTH_OK) {
                strlcpy(c->access, key, sizeof(c->access));
                c->refresh[0] = c->username[0] = 0;
            }
        } else if (c->access[0] && !c->username[0] && !c->refresh[0] &&
                   strcmp(server, c->server) == 0) {
            set_status(true, "Checking the saved API key...");
            r = abs_check(server, c->access);
        } else {
            set_status(false, "Enter an API key for the new sign-in.");
            goto out;
        }
    } else if (strcmp(mode, "password") == 0) {
        if (user[0] && pass[0]) {
            set_status(true, "Signing in as %s...", user);
            // Never let a refresh token from the previous account survive a successful user switch.
            c->access[0] = 0;
            c->refresh[0] = 0;
            r = abs_login(server, user, pass, c->access, sizeof(c->access), c->refresh, sizeof(c->refresh));
            if (r == ABS_AUTH_OK) strlcpy(c->username, user, sizeof(c->username));
        } else if (c->access[0] && c->username[0] && strcmp(user, c->username) == 0 &&
                   strcmp(server, c->server) == 0) {
            set_status(true, "Checking the saved sign-in...");
            r = abs_check(server, c->access);
            // An expired access token with a refresh token is fine: it renews on first use.
            if (r == ABS_AUTH_REJECTED && c->refresh[0]) r = ABS_AUTH_OK;
        } else {
            set_status(false, user[0] && strcmp(user, c->username) != 0
                                  ? "Enter the password for the new Audiobookshelf user."
                                  : "Enter your username and password.");
            goto out;
        }
    } else {
        set_status(false, "Choose username/password or API key sign-in.");
        goto out;
    }
    if (r != ABS_AUTH_OK) {
        set_status(false, r == ABS_AUTH_UNREACHABLE ? "Couldn't reach %s. Check the address."
                          : r == ABS_AUTH_REJECTED  ? "The server didn't accept those details."
                                                    : "Unexpected reply from %s.",
                   server);
        goto out;
    }
    strlcpy(c->server, server, sizeof(c->server));

    // Device preferences.
    power_config_t pc;
    power_get_config(&pc);
    pc.brightness = num(req, "brightness", pc.brightness);
    pc.idle_timeout_minutes = decimal(req, "idle_timeout_min", pc.idle_timeout_minutes);
    pc.screen_saver_mode = num(req, "screen_saver_mode", pc.screen_saver_mode);
    pc.screen_saver_dim_percent = num(req, "screen_saver_dim_percent", pc.screen_saver_dim_percent);
    const cJSON *power_save = cJSON_GetObjectItemCaseSensitive(req, "power_save_enabled");
    if (cJSON_IsBool(power_save)) pc.power_save_enabled = cJSON_IsTrue(power_save);
    power_set_config(&pc);
    c->knob_hold_s = decimal(req, "knob_hold_s", c->knob_hold_s);
    c->acceleration_multiplier = num(req, "acceleration_multiplier", c->acceleration_multiplier);
    c->acceleration_threshold_ms = num(req, "acceleration_threshold_ms", c->acceleration_threshold_ms);
    c->marquee_delay_ms = num(req, "marquee_delay_ms", c->marquee_delay_ms);
    c->marquee_speed_px_s = num(req, "marquee_speed_px_s", c->marquee_speed_px_s);
    c->marquee_loop_pause_ms = num(req, "marquee_loop_pause_ms", c->marquee_loop_pause_ms);
    c->browse_return_s = num(req, "browse_return_s", c->browse_return_s);
    c->playback_speed_pct = num(req, "playback_speed_pct", c->playback_speed_pct);
    c->library_sort_mode = num(req, "library_sort_mode", c->library_sort_mode);
    c->series_sort_mode = num(req, "series_sort_mode", c->series_sort_mode);
    c->author_sort_mode = num(req, "author_sort_mode", c->author_sort_mode);
    c->sleep_timer_minutes = num(req, "sleep_timer_minutes", c->sleep_timer_minutes);
    c->display_type = num(req, "display_type", c->display_type);
    const cJSON *sleep_timer = cJSON_GetObjectItemCaseSensitive(req, "sleep_timer_enabled");
    if (cJSON_IsBool(sleep_timer)) c->sleep_timer_enabled = cJSON_IsTrue(sleep_timer);
    c->root_enabled_mask = num(req, "root_enabled_mask", c->root_enabled_mask);
    const cJSON *resume = cJSON_GetObjectItemCaseSensitive(req, "resume_last_at_startup");
    if (cJSON_IsBool(resume)) c->resume_last_at_startup = cJSON_IsTrue(resume);
    const cJSON *order = cJSON_GetObjectItemCaseSensitive(req, "root_order");
    if (cJSON_IsArray(order) && cJSON_GetArraySize(order) == 6) {
        for (int i = 0; i < 6; ++i) {
            const cJSON *position = cJSON_GetArrayItem(order, i);
            if (cJSON_IsNumber(position)) c->root_order[i] = (uint8_t)position->valueint;
        }
    }

    const bool account_changed = strcmp(previous_config->server, c->server) != 0 ||
                                 strcmp(previous_config->username, c->username) != 0 ||
                                 ((!previous_config->username[0] || !c->username[0]) &&
                                  strcmp(previous_config->access, c->access) != 0);
    const bool connection_changed = strcmp(previous_config->wifi_ssid, c->wifi_ssid) != 0 ||
                                    strcmp(previous_config->wifi_pass, c->wifi_pass) != 0 ||
                                    strcmp(previous_config->server, c->server) != 0 ||
                                    strcmp(previous_config->username, c->username) != 0 ||
                                    strcmp(previous_config->access, c->access) != 0 ||
                                    strcmp(previous_config->refresh, c->refresh) != 0;
    const bool display_changed = previous_config->display_type != c->display_type;
    if (account_changed && !catalog_prepare_account_switch()) {
        catalog_cancel_account_switch();
        set_status(false, "Account was not changed because the old library cache could not be cleared.");
        goto out;
    }
    if (!config_save(c)) {
        if (account_changed) catalog_cancel_account_switch();
        set_status(false, "Settings could not be saved to device storage. No restart was made.");
        goto out;
    }
    player_set_playback_speed(c->playback_speed_pct);
    ui_settings_changed();
    if (connection_changed || display_changed) {
        set_status(true, account_changed
                             ? "Account changed. Restarting to load its library..."
                             : display_changed
                                   ? "All set. Restarting to apply the display change..."
                                   : "All set. Restarting to apply connection changes...");
        vTaskDelay(pdMS_TO_TICKS(2500));  // let the page show it
        esp_restart();
    }
    set_status(false, "Settings saved.");
out:
    free(c);
}

static void apply_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_bring_up) {
            s_bring_up = false;
            if (!s_scan) s_scan = heap_caps_calloc(MAX_SCAN, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
            s_scan_count = s_scan ? wifi_scan(s_scan, MAX_SCAN) : 0;
            ESP_LOGI(TAG, "%d networks nearby", s_scan_count);
            if (s_setup_active) {
                if (wifi_start_ap(s_ssid, s_pass) == ESP_OK) set_status(false, "Waiting for your phone or laptop");
                else set_status(false, "Couldn't start the setup network.");
            }
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cJSON *req = s_pending;
        s_pending = NULL;
        xSemaphoreGive(s_lock);
        if (req) apply(req);
        cJSON_Delete(req);
    }
}

static bool ensure_apply_worker(void)
{
    if (s_apply_task) return true;
    BaseType_t result = xTaskCreatePinnedToCoreWithCaps(apply_task, "setup", 8192, NULL, 3,
                                                        &s_apply_task, 0, MALLOC_CAP_SPIRAM);
    if (result != pdPASS || !s_apply_task) {
        s_apply_task = NULL;
        return false;
    }
    return true;
}

/* ---------- public ---------- */

static void initialize_portal_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

static esp_err_t start_http_server(void)
{
    if (s_active) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 16;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start");
        s_httpd = NULL;
        return ESP_FAIL;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = on_page},
        {.uri = "/api/config", .method = HTTP_GET, .handler = on_config},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = on_scan},
        {.uri = "/api/status", .method = HTTP_GET, .handler = on_status},
        {.uri = "/api/player", .method = HTTP_GET, .handler = on_player},
        {.uri = "/api/books", .method = HTTP_GET, .handler = on_books},
        {.uri = "/api/recent", .method = HTTP_GET, .handler = on_recent},
        {.uri = "/api/play", .method = HTTP_POST, .handler = on_play},
        {.uri = "/api/control", .method = HTTP_POST, .handler = on_control},
        {.uri = "/api/save", .method = HTTP_POST, .handler = on_save},
        {.uri = "/*", .method = HTTP_GET, .handler = on_other},
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_httpd, &uris[i]);
    s_active = true;
    return ESP_OK;
}

void portal_start_station(void)
{
    initialize_portal_lock();
    if (!ensure_apply_worker()) {
        ESP_LOGE(TAG, "couldn't start the settings worker");
        return;
    }
    if (start_http_server() == ESP_OK) ESP_LOGI(TAG, "web portal started on the Wi-Fi interface");
}

void portal_start(void)
{
    if (s_setup_active) return;
    initialize_portal_lock();
    if (!ensure_apply_worker()) {
        set_status(false, "Couldn't start setup service.");
        return;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "Chapter-Setup-%02X%02X", mac[4], mac[5]);
    snprintf(s_pass, sizeof(s_pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
    if (start_http_server() != ESP_OK) return;
    // The setup task scans for networks first, then brings the access point up (a scan takes
    // seconds, so keep it off the caller, which holds the UI lock).
    s_setup_active = true;
    set_status(true, "Starting the setup network...");
    s_scan_count = -1;
    s_bring_up = true;
    xTaskNotifyGive(s_apply_task);

    while (s_dns_task) vTaskDelay(pdMS_TO_TICKS(20));
    s_dns_stop = false;
    xTaskCreatePinnedToCoreWithCaps(dns_task, "dns", 4096, NULL, 3, &s_dns_task, 0, MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "setup portal on '%s' password '%s' (internal RAM free %u)", s_ssid, s_pass,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void portal_stop_setup(void)
{
    if (!s_setup_active) return;
    s_setup_active = false;
    s_bring_up = false;
    s_dns_stop = true;
    wifi_stop_ap();
}

void portal_stop(void)
{
    portal_stop_setup();
    if (!s_active) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    s_active = false;
}

bool portal_active(void)
{
    return s_active;
}

bool portal_setup_active(void)
{
    return s_setup_active;
}

void portal_network(char *ssid, size_t slen, char *pass, size_t plen)
{
    strlcpy(ssid, s_ssid, slen);
    strlcpy(pass, s_pass, plen);
}

void portal_status(char *msg, size_t len, bool *busy)
{
    if (!s_lock) {
        msg[0] = 0;
        *busy = false;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(msg, s_message, len);
    *busy = s_busy;
    xSemaphoreGive(s_lock);
}
