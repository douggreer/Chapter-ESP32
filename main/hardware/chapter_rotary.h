#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    CHAPTER_KNOB_SELECT = 0,
    CHAPTER_KNOB_VOLUME = 1,
    CHAPTER_KNOB_COUNT = 2,
} chapter_knob_t;

typedef struct {
    int detents;       // signed; positive is clockwise when looking at the knob face
    bool pressed;      // debounced level
    bool pressed_edge; // one poll on the down transition
    bool released_edge;// one poll on the up transition
} chapter_knob_sample_t;

esp_err_t chapter_rotary_init(void);
void chapter_rotary_poll(chapter_knob_t knob, chapter_knob_sample_t *sample);
