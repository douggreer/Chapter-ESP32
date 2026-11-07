#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

esp_err_t chapter_display_init(void);
int chapter_display_width(void);
int chapter_display_height(void);
void chapter_display_set_power(bool enabled);
void chapter_display_set_brightness(unsigned percent);
void chapter_display_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels);
