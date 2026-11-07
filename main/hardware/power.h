#pragma once

#include <stdbool.h>

typedef struct {
    int brightness;       // OLED contrast, 10-100 percent
    float idle_timeout_minutes; // shared screen saver and power-save timeout; 0 disables both
    int screen_saver_mode; // 0=chapter, 1=books, 2=clock, 3=off
    int screen_saver_dim_percent;
    bool power_save_enabled;
} power_config_t;

void power_init(void);
void power_get_config(power_config_t *out);
void power_set_config(const power_config_t *config);
void power_note_activity(void);
bool power_screen_saver_active(void);
void power_apply_screen_saver(bool active);
void power_service(void);
void power_shutdown_now(void);
