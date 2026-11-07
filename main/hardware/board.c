// Chapter hardware support: selectable SH1122/SSD1306 OLED and MAX98357A I2S amplifier; no touch panel or external codec.

#include "board.h"

#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "chapter_display.h"
#include "chapter_pins.h"

static const char *TAG = "chapter_board";
static i2s_chan_handle_t s_i2s_tx;
static bool s_audio_enabled;
static bool s_amp_gpio_ready;
static int s_rate = 44100;
static int s_channels = 2;
static int s_vol_q15 = 32768 / 2;

esp_err_t board_display_init(void)
{
    return chapter_display_init();
}

void board_set_brightness(int percent)
{
    chapter_display_set_brightness(percent);
}

void board_display_power(bool on)
{
    chapter_display_set_power(on);
}

void board_prepare_deep_sleep(void)
{
    board_audio_close();
    board_display_power(false);
    if (s_amp_gpio_ready) {
        gpio_set_level(CHAPTER_PIN_AMP_SD, 0);
        // ESP32-S3 resets ordinary digital GPIO state in deep sleep unless both the pad
        // hold and the global deep-sleep hold are enabled. Keep the amp in shutdown.
        gpio_hold_en(CHAPTER_PIN_AMP_SD);
        gpio_deep_sleep_hold_en();
    }
}

esp_err_t board_audio_init(void)
{
    // Deep-sleep wake restarts the app; release the amp pad before configuring it again.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(CHAPTER_PIN_AMP_SD);
    const gpio_config_t amp = {
        .pin_bit_mask = 1ULL << CHAPTER_PIN_AMP_SD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&amp), TAG, "amp SD pin setup failed");
    s_amp_gpio_ready = true;
    gpio_set_level(CHAPTER_PIN_AMP_SD, 0); // MAX98357A shutdown is active low.

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    channel.dma_desc_num = 8;
    channel.dma_frame_num = 512;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &s_i2s_tx, NULL), TAG, "I2S channel setup failed");

    const i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = CHAPTER_PIN_I2S_BCLK,
            .ws = CHAPTER_PIN_I2S_LRCLK,
            .dout = CHAPTER_PIN_I2S_DOUT,
            .din = GPIO_NUM_NC,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &config), TAG, "I2S mode setup failed");
    ESP_LOGI(TAG, "MAX98357A audio output ready");
    return ESP_OK;
}

esp_err_t board_audio_open(int sample_rate, int channels, int bits)
{
    ESP_RETURN_ON_FALSE(s_i2s_tx && bits == 16 && (channels == 1 || channels == 2),
                        ESP_ERR_INVALID_ARG, TAG, "unsupported audio format %d/%d/%d", sample_rate, channels, bits);
    if (s_audio_enabled && sample_rate == s_rate && channels == s_channels) return ESP_OK;
    board_audio_close();

    const i2s_std_clk_config_t clock = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    const i2s_std_slot_config_t slots = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO);
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_i2s_tx, &clock), TAG, "I2S clock setup failed");
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_slot(s_i2s_tx, &slots), TAG, "I2S slot setup failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "I2S enable failed");
    gpio_set_level(CHAPTER_PIN_AMP_SD, 1);
    s_rate = sample_rate;
    s_channels = channels;
    s_audio_enabled = true;
    return ESP_OK;
}

void board_audio_close(void)
{
    if (s_audio_enabled) {
        i2s_channel_disable(s_i2s_tx);
        s_audio_enabled = false;
    }
    if (s_i2s_tx) gpio_set_level(CHAPTER_PIN_AMP_SD, 0);
}

esp_err_t board_audio_write(int16_t *samples, int len)
{
    if (!s_audio_enabled) return ESP_ERR_INVALID_STATE;
    const int count = len / (int)sizeof(int16_t);
    for (int i = 0; i < count; ++i) {
        samples[i] = (int16_t)(((int32_t)samples[i] * s_vol_q15) >> 15);
    }
    size_t written = 0;
    return i2s_channel_write(s_i2s_tx, samples, len, &written, portMAX_DELAY);
}

void board_audio_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    // A squared curve gives a useful adjustment range at low volume.
    s_vol_q15 = (32768 * volume * volume) / 10000;
}
