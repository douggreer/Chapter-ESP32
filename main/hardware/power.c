// Battery idle policy for Chapter: stop Wi-Fi, switch off the OLED and MAX98357A, then deep-sleep.

#include "power.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "board.h"
#include "chapter_pins.h"
#include "config.h"
#include "player.h"
#include "portal.h"
#include "wifi.h"

static const char *TAG = "chapter_power";
static power_config_t s_config = {
    .brightness = 80, .idle_timeout_minutes = 5, .screen_saver_mode = 0,
    .screen_saver_dim_percent = 15, .power_save_enabled = true,
};
static int64_t s_last_activity_us;
static player_state_t s_last_player_state = PLAYER_IDLE;
static bool s_screen_saver_applied;

static const gpio_num_t s_wake_pins[] = {
    CHAPTER_PIN_SELECT_A, CHAPTER_PIN_SELECT_SW,
    CHAPTER_PIN_VOLUME_A, CHAPTER_PIN_VOLUME_SW,
};

static void save_settings(void *unused)
{
    (void)unused;
    nvs_handle_t handle;
    if (nvs_open("chapter_power", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, "bright", (uint8_t)s_config.brightness);
    nvs_set_u16(handle, "idle_ds", (uint16_t)(s_config.idle_timeout_minutes * 10.0f + 0.5f));
    nvs_set_u8(handle, "saver_mode", (uint8_t)s_config.screen_saver_mode);
    nvs_set_u8(handle, "saver_dim", (uint8_t)s_config.screen_saver_dim_percent);
    nvs_set_u8(handle, "power_save", s_config.power_save_enabled);
    nvs_commit(handle);
    nvs_close(handle);
}

void power_get_config(power_config_t *out)
{
    *out = s_config;
}

void power_set_config(const power_config_t *config)
{
    s_config = *config;
    if (s_config.brightness < 10) s_config.brightness = 10;
    if (s_config.brightness > 100) s_config.brightness = 100;
    if (s_config.idle_timeout_minutes < 0) s_config.idle_timeout_minutes = 0;
    if (s_config.idle_timeout_minutes > 60) s_config.idle_timeout_minutes = 60;
    s_config.idle_timeout_minutes = ((int)(s_config.idle_timeout_minutes * 2.0f + 0.5f)) / 2.0f;
    if (s_config.screen_saver_mode < 0 || s_config.screen_saver_mode > 3) s_config.screen_saver_mode = 0;
    if (s_config.screen_saver_dim_percent < 10) s_config.screen_saver_dim_percent = 10;
    if (s_config.screen_saver_dim_percent > 50) s_config.screen_saver_dim_percent = 50;
    s_last_activity_us = esp_timer_get_time();
    if (s_screen_saver_applied) power_apply_screen_saver(false);
    else {
        board_display_power(true);
        board_set_brightness(s_config.brightness);
    }
    flash_safe(save_settings, NULL);
}

void power_note_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

bool power_screen_saver_active(void)
{
    if (s_config.power_save_enabled || !s_config.idle_timeout_minutes || portal_setup_active()) return false;
    player_status_t status;
    player_get_status(&status);
    if (status.state == PLAYER_PLAYING || status.state == PLAYER_BUFFERING || status.state == PLAYER_LOADING) return false;
    const int64_t timeout = (int64_t)(s_config.idle_timeout_minutes * 60 * 1000000.0f);
    return esp_timer_get_time() - s_last_activity_us >= timeout;
}

void power_apply_screen_saver(bool active)
{
    if (s_screen_saver_applied == active) return;
    s_screen_saver_applied = active;
    if (active && s_config.screen_saver_mode == 3) {
        board_display_power(false);
        return;
    }
    board_display_power(true);
    board_set_brightness(active ? s_config.screen_saver_dim_percent : s_config.brightness);
}

void power_init(void)
{
    nvs_handle_t handle;
    if (nvs_open("chapter_power", NVS_READONLY, &handle) == ESP_OK) {
        uint8_t brightness;
        uint16_t idle;
        if (nvs_get_u8(handle, "bright", &brightness) == ESP_OK) s_config.brightness = brightness;
        if (nvs_get_u16(handle, "idle_ds", &idle) == ESP_OK) s_config.idle_timeout_minutes = idle / 10.0f;
        else if (nvs_get_u16(handle, "idle_min", &idle) == ESP_OK) s_config.idle_timeout_minutes = idle;
        uint8_t byte;
        if (nvs_get_u8(handle, "saver_mode", &byte) == ESP_OK) s_config.screen_saver_mode = byte;
        if (nvs_get_u8(handle, "saver_dim", &byte) == ESP_OK) s_config.screen_saver_dim_percent = byte;
        if (nvs_get_u8(handle, "power_save", &byte) == ESP_OK) s_config.power_save_enabled = byte != 0;
        nvs_close(handle);
    }
    board_set_brightness(s_config.brightness);
    s_last_activity_us = esp_timer_get_time();
}

static bool player_active(void)
{
    player_status_t state;
    player_get_status(&state);
    return state.state == PLAYER_PLAYING || state.state == PLAYER_BUFFERING || state.state == PLAYER_LOADING;
}

static void set_wake_gpio(gpio_num_t pin)
{
    if (!rtc_gpio_is_valid_gpio(pin)) return;
    rtc_gpio_init(pin);
    rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(pin);
    rtc_gpio_pulldown_dis(pin);
}

static void enter_deep_sleep(bool stop_current_playback)
{
    if (stop_current_playback) {
        player_stop();
        const int64_t deadline = esp_timer_get_time() + 10000000LL;
        while (esp_timer_get_time() < deadline) {
            player_status_t status;
            player_get_status(&status);
            if (status.state == PLAYER_IDLE || status.state == PLAYER_FINISHED || status.state == PLAYER_ERROR) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (portal_active()) portal_stop();
    else wifi_stop_ap();

    // Do not sleep while a knob is still held: its active-low switch would wake the chip immediately.
    for (;;) {
        bool held = false;
        for (size_t i = 0; i < sizeof(s_wake_pins) / sizeof(s_wake_pins[0]); ++i) {
            if (gpio_get_level(s_wake_pins[i]) == 0) held = true;
        }
        if (!held) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "entering deep sleep; a knob turn or click will wake Chapter");
    board_prepare_deep_sleep();
    esp_wifi_stop();

    uint64_t wake_mask = 0;
    for (size_t i = 0; i < sizeof(s_wake_pins) / sizeof(s_wake_pins[0]); ++i) {
        set_wake_gpio(s_wake_pins[i]);
        wake_mask |= 1ULL << s_wake_pins[i];
    }
    esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

void power_shutdown_now(void)
{
    enter_deep_sleep(true);
}

void power_service(void)
{
    player_status_t status;
    player_get_status(&status);
    const bool now_active = status.state == PLAYER_PLAYING || status.state == PLAYER_BUFFERING ||
                            status.state == PLAYER_LOADING;
    if (now_active || s_last_player_state == PLAYER_PLAYING ||
        s_last_player_state == PLAYER_BUFFERING || s_last_player_state == PLAYER_LOADING) {
        s_last_activity_us = esp_timer_get_time();
    }
    s_last_player_state = status.state;

    if (!s_config.power_save_enabled || !s_config.idle_timeout_minutes || now_active || portal_setup_active()) return;
    const int64_t timeout = (int64_t)(s_config.idle_timeout_minutes * 60 * 1000000.0f);
    if (esp_timer_get_time() - s_last_activity_us >= timeout) enter_deep_sleep(false);
}
