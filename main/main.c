#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "abs_api.h"
#include "board.h"
#include "catalog.h"
#include "config.h"
#include "download.h"
#include "player.h"
#include "power.h"
#include "portal.h"
#include "storage.h"
#include "ui.h"
#include "wifi.h"

#define RETRY_US (30 * 1000000LL)

static const char *TAG = "chapter_main";

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    initialize_nvs();
    config_init();
    ESP_LOGI(TAG, "reset reason %d", (int)esp_reset_reason());

    ui_init();
    if (board_audio_init() != ESP_OK) ESP_LOGE(TAG, "speaker amplifier initialization failed");
    storage_init();
    power_init();
    abs_api_init();
    player_init();
    download_init();
    wifi_start();
    catalog_init();

    if (!config_complete()) {
        portal_start();
        ui_setup_show();
    } else {
        portal_start_station();
        ui_show_message("Connecting to Wi-Fi...");
    }

    bool cached = catalog_load_cached();
    // A saved catalog is immediately usable. Don't leave a Wi-Fi/connectivity splash over it while
    // the background Audiobookshelf refresh is attempted.
    if (cached) ui_show_message(NULL);
    bool online = false;
    bool first_network_load = true;
    bool signed_out_notice = false;
    int64_t last_try = -RETRY_US;
    const int64_t started = esp_timer_get_time();

    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (config_complete() && !online && wifi_is_connected() && now - last_try >= RETRY_US) {
            ui_show_message(cached ? "Refreshing Audiobookshelf library..."
                                   : "Loading Audiobookshelf library...");
            online = catalog_load_network(cached);
            last_try = now;
            if (online) {
                if (first_network_load) {
                    first_network_load = false;
                    ESP_LOGI(TAG, "library loaded: internal RAM %u bytes, PSRAM %u bytes",
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                }
                ui_show_message(NULL);
            }
        } else if (config_complete() && !online && !cached && !wifi_is_connected() &&
                   now - started > 15000000LL && !portal_setup_active()) {
            ui_show_message("Waiting for Wi-Fi...\nOpen setup portal to change it");
        }

        char library[40];
        if (ui_take_library_request(library, sizeof(library))) {
            char previous[40];
            strlcpy(previous, catalog_library_id(), sizeof(previous));
            catalog_select(library);
            cached = catalog_load_cached();
            if (wifi_is_connected()) {
                online = catalog_load_network(cached);
                last_try = now;
            } else if (!cached) {
                catalog_select(previous);
                ui_show_message("That library is not cached.\nConnect to Wi-Fi first.");
            }
        }

        if (ui_take_refresh_request()) {
            if (wifi_is_connected()) {
                online = catalog_load_network(false);
                last_try = now;
                if (online) ui_show_message(NULL);
            } else {
                ui_show_message("Offline - showing saved library");
            }
        }

        if (abs_signed_out() && !signed_out_notice) {
            signed_out_notice = true;
            ui_show_message("Audiobookshelf sign-in expired\nReconnect in setup portal");
        }

        ui_process();
        if (ui_take_shutdown_request()) power_shutdown_now();
        power_service();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
