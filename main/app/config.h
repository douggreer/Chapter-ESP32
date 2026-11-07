#pragma once

// Device configuration, stored in NVS and set up from a phone through the setup portal
// (portal.c). Nothing is built in: a new device starts in setup.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CHAPTER_DISPLAY_SH1122 = 0,
    CHAPTER_DISPLAY_SSD1306 = 1,
} chapter_display_type_t;

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char server[128];     // base URL, e.g. https://abs.example.com or http://192.168.1.5:13378
    char username[64];    // who signed in (display only; "" when using an API key)
    char access[1024];    // Bearer credential: an API key, or an access token from signing in
    char refresh[1024];   // refresh token for renewing the access token ("" for API keys)
    float knob_hold_s;
    int acceleration_multiplier; // 0=off; otherwise 2-10
    int acceleration_threshold_ms;
    int marquee_delay_ms;
    int marquee_speed_px_s;
    int marquee_loop_pause_ms;
    int browse_return_s;
    int playback_speed_pct; // 100, 125, 150, or 200
    uint8_t library_sort_mode; // 0=title, 1=author
    uint8_t sleep_timer_minutes;
    bool sleep_timer_enabled;
    uint8_t display_type; // chapter_display_type_t; changing it requires a restart
    uint8_t root_enabled_mask;
    uint8_t root_order[6]; // IDs: Continue, Series, Title, Author, Bluetooth (omitted), Shutdown
    bool resume_last_at_startup;
} app_config_t;

void config_init(void);
// Read-only view; valid for the life of the program (contents change only via the setters).
const app_config_t *config_get(void);
// Replaces and saves the whole configuration.
bool config_save(const app_config_t *cfg);
// Saves new tokens after a refresh (thread-safe).
void config_set_tokens(const char *access, const char *refresh);
// Wi-Fi, a server and a credential are all present.
bool config_complete(void);

// Runs fn(arg) where flash (NVS) access is allowed, and waits for it. Flash operations disable
// the cache PSRAM sits behind, so a task with a PSRAM stack can't do them itself: from such a
// task this borrows a short-lived task with an internal-RAM stack.
void flash_safe(void (*fn)(void *), void *arg);
