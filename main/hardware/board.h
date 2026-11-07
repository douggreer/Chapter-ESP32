#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t board_display_init(void);
void board_set_brightness(int percent);
void board_display_power(bool on);
void board_prepare_deep_sleep(void);

esp_err_t board_audio_init(void);
esp_err_t board_audio_open(int sample_rate, int channels, int bits);
void board_audio_close(void);
esp_err_t board_audio_write(int16_t *samples, int len);
void board_audio_set_volume(int volume);
