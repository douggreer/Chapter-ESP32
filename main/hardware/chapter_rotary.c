#include "chapter_rotary.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_timer.h"
#include "chapter_pins.h"

#define ENCODER_DEBOUNCE_US 25000LL
#define ENCODER_EDGES_PER_DETENT 4

typedef struct {
    gpio_num_t a;
    gpio_num_t b;
    gpio_num_t sw;
    uint8_t previous_ab;
    int quarter_steps;
    bool button_raw_down;
    bool button_down;
    int64_t button_changed_at;
} encoder_state_t;

static encoder_state_t s_encoders[CHAPTER_KNOB_COUNT] = {
    {.a = CHAPTER_PIN_SELECT_A, .b = CHAPTER_PIN_SELECT_B, .sw = CHAPTER_PIN_SELECT_SW},
    {.a = CHAPTER_PIN_VOLUME_A, .b = CHAPTER_PIN_VOLUME_B, .sw = CHAPTER_PIN_VOLUME_SW},
};

// Gray-code transition table; invalid two-bit jumps contribute no movement.
static const int8_t s_quadrature[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

static uint8_t read_ab(const encoder_state_t *encoder)
{
    return (uint8_t)((gpio_get_level(encoder->a) << 1) | gpio_get_level(encoder->b));
}

esp_err_t chapter_rotary_init(void)
{
    uint64_t pins = 0;
    for (int i = 0; i < CHAPTER_KNOB_COUNT; ++i) {
        pins |= 1ULL << s_encoders[i].a;
        pins |= 1ULL << s_encoders[i].b;
        pins |= 1ULL << s_encoders[i].sw;
    }
    const gpio_config_t config = {
        .pin_bit_mask = pins,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < CHAPTER_KNOB_COUNT; ++i) {
        const gpio_num_t pins[] = {s_encoders[i].a, s_encoders[i].b, s_encoders[i].sw};
        for (size_t j = 0; j < sizeof(pins) / sizeof(pins[0]); ++j) {
            if (rtc_gpio_is_valid_gpio(pins[j])) rtc_gpio_deinit(pins[j]);
        }
    }
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;

    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < CHAPTER_KNOB_COUNT; ++i) {
        encoder_state_t *encoder = &s_encoders[i];
        encoder->previous_ab = read_ab(encoder);
        encoder->button_raw_down = gpio_get_level(encoder->sw) == 0;
        encoder->button_down = encoder->button_raw_down;
        encoder->button_changed_at = now;
    }
    return ESP_OK;
}

void chapter_rotary_poll(chapter_knob_t knob, chapter_knob_sample_t *sample)
{
    memset(sample, 0, sizeof(*sample));
    if (knob < 0 || knob >= CHAPTER_KNOB_COUNT) return;

    encoder_state_t *encoder = &s_encoders[knob];
    const uint8_t current = read_ab(encoder);
    const unsigned transition = ((unsigned)encoder->previous_ab << 2) | current;
    encoder->quarter_steps += s_quadrature[transition];
    encoder->previous_ab = current;
    if (encoder->quarter_steps >= ENCODER_EDGES_PER_DETENT) {
        sample->detents = 1;
        encoder->quarter_steps -= ENCODER_EDGES_PER_DETENT;
    } else if (encoder->quarter_steps <= -ENCODER_EDGES_PER_DETENT) {
        sample->detents = -1;
        encoder->quarter_steps += ENCODER_EDGES_PER_DETENT;
    }

    const int64_t now = esp_timer_get_time();
    const bool raw_down = gpio_get_level(encoder->sw) == 0;
    if (raw_down != encoder->button_raw_down) {
        encoder->button_raw_down = raw_down;
        encoder->button_changed_at = now;
    } else if (raw_down != encoder->button_down &&
               now - encoder->button_changed_at >= ENCODER_DEBOUNCE_US) {
        encoder->button_down = raw_down;
        sample->pressed_edge = raw_down;
        sample->released_edge = !raw_down;
    }
    sample->pressed = encoder->button_down;
}
