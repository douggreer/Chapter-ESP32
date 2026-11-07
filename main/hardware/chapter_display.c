#include "chapter_display.h"

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "chapter_pins.h"
#include "config.h"

#define SH1122_WIDTH 256
#define SSD1306_WIDTH 128
#define OLED_HEIGHT 64
#define SH1122_PACKED_BYTES (SH1122_WIDTH * OLED_HEIGHT / 2)
#define SSD1306_FRAME_BYTES (SSD1306_WIDTH * OLED_HEIGHT / 8)
#define SSD1306_I2C_ADDRESS_1 0x3C
#define SSD1306_I2C_ADDRESS_2 0x3D

static const char *TAG = "chapter_oled";
static spi_device_handle_t s_spi;
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_i2c_device;
static bool s_ssd1306;
static bool s_enabled;
static DMA_ATTR uint8_t s_sh1122_pixels[SH1122_PACKED_BYTES] __attribute__((aligned(4)));
static uint8_t s_ssd1306_pixels[SSD1306_FRAME_BYTES];

static esp_err_t spi_bytes(bool data, const void *buffer, size_t length)
{
    gpio_set_level(CHAPTER_PIN_OLED_CS, 0);
    gpio_set_level(CHAPTER_PIN_OLED_DC, data ? 1 : 0);

    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = buffer,
    };
    esp_err_t err = spi_device_polling_transmit(s_spi, &transaction);

    gpio_set_level(CHAPTER_PIN_OLED_CS, 1);
    return err;
}

static esp_err_t i2c_bytes(bool data, const uint8_t *bytes, size_t length)
{
    uint8_t packet[32];
    if (length > sizeof(packet) - 1) return ESP_ERR_INVALID_SIZE;
    packet[0] = data ? 0x40 : 0x00;
    memcpy(packet + 1, bytes, length);
    return i2c_master_transmit(s_i2c_device, packet, length + 1, 100);
}

static esp_err_t send_commands(const uint8_t *bytes, size_t length)
{
    if (s_ssd1306) return i2c_bytes(false, bytes, length);
    return spi_bytes(false, bytes, length);
}

static esp_err_t command(uint8_t value)
{
    return send_commands(&value, 1);
}

static esp_err_t init_sh1122(void)
{
    const gpio_config_t output = {
        .pin_bit_mask = (1ULL << CHAPTER_PIN_OLED_CS) |
                        (1ULL << CHAPTER_PIN_OLED_DC) |
                        (1ULL << CHAPTER_PIN_OLED_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output), TAG, "OLED GPIO setup failed");
    gpio_set_level(CHAPTER_PIN_OLED_CS, 1);
    gpio_set_level(CHAPTER_PIN_OLED_DC, 0);
    gpio_set_level(CHAPTER_PIN_OLED_RST, 1);

    const spi_bus_config_t bus = {
        .mosi_io_num = CHAPTER_PIN_OLED_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CHAPTER_PIN_OLED_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SH1122_PACKED_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG,
                        "SPI bus setup failed");

    const spi_device_interface_config_t device = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &device, &s_spi), TAG,
                        "SPI device setup failed");

    gpio_set_level(CHAPTER_PIN_OLED_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CHAPTER_PIN_OLED_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // SH1122 initialization for the Chapter 256x64 panel.
    static const uint8_t init[] = {
        0xAE, 0x00, 0x10, 0x40, 0xA0, 0xA8, 0x3F, 0xC0,
        0xD3, 0x00, 0x81, 0x80, 0xA4, 0xA6, 0xAF,
    };
    ESP_RETURN_ON_ERROR(spi_bytes(false, init, sizeof(init)), TAG, "SH1122 init failed");
    s_enabled = true;
    ESP_LOGI(TAG, "SH1122 256x64 SPI display initialized");
    return ESP_OK;
}

static esp_err_t init_ssd1306(void)
{
    const i2c_master_bus_config_t bus = {
        .i2c_port = -1,
        .sda_io_num = CHAPTER_PIN_OLED_I2C_SDA,
        .scl_io_num = CHAPTER_PIN_OLED_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c_bus), TAG, "I2C bus setup failed");

    uint16_t address = SSD1306_I2C_ADDRESS_1;
    esp_err_t probe = i2c_master_probe(s_i2c_bus, address, 100);
    if (probe != ESP_OK) {
        address = SSD1306_I2C_ADDRESS_2;
        probe = i2c_master_probe(s_i2c_bus, address, 100);
    }
    if (probe != ESP_OK) {
        ESP_LOGE(TAG, "No SSD1306 acknowledged at I2C address 0x3C or 0x3D");
        return probe;
    }

    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &device, &s_i2c_device), TAG,
                        "SSD1306 I2C device setup failed");

    static const uint8_t init[] = {
        0xAE,             // Display off
        0xD5, 0x80,       // Clock divide
        0xA8, 0x3F,       // 64-row multiplex
        0xD3, 0x00,       // Display offset
        0x40,             // Start line 0
        0x8D, 0x14,       // Enable charge pump
        0x20, 0x00,       // Horizontal addressing
        0xA1, 0xC8,       // Segment remap and COM scan direction
        0xDA, 0x12,       // 128x64 COM pins
        0x81, 0x7F,       // Contrast
        0xD9, 0xF1,       // Pre-charge
        0xDB, 0x40,       // VCOMH deselect
        0xA4, 0xA6, 0xAF, // Resume RAM, normal display, on
    };
    ESP_RETURN_ON_ERROR(i2c_bytes(false, init, sizeof(init)), TAG, "SSD1306 init failed");
    s_enabled = true;
    ESP_LOGI(TAG, "SSD1306 128x64 I2C display initialized at 0x%02X", address);
    return ESP_OK;
}

esp_err_t chapter_display_init(void)
{
    s_ssd1306 = config_get()->display_type == CHAPTER_DISPLAY_SSD1306;
    return s_ssd1306 ? init_ssd1306() : init_sh1122();
}

int chapter_display_width(void)
{
    return s_ssd1306 ? SSD1306_WIDTH : SH1122_WIDTH;
}

int chapter_display_height(void)
{
    return OLED_HEIGHT;
}

void chapter_display_set_power(bool enabled)
{
    if ((!s_spi && !s_i2c_device) || s_enabled == enabled) return;
    if (command(enabled ? 0xAF : 0xAE) == ESP_OK) s_enabled = enabled;
}

void chapter_display_set_brightness(unsigned percent)
{
    if (percent > 100) percent = 100;
    // Keep OLED contrast responsive at low settings while avoiding a completely black panel.
    const uint8_t contrast = (uint8_t)((percent * percent * 0x80U) / 10000U);
    const uint8_t pair[] = {0x81, contrast};
    if (s_spi || s_i2c_device) send_commands(pair, sizeof(pair));
}

static uint8_t rgb565_to_gray4(uint16_t pixel)
{
    const unsigned r = ((pixel >> 11) & 0x1f) * 255U / 31U;
    const unsigned g = ((pixel >> 5) & 0x3f) * 255U / 63U;
    const unsigned b = (pixel & 0x1f) * 255U / 31U;
    const unsigned gray = (77U * r + 150U * g + 29U * b) >> 8;
    return (uint8_t)(gray >> 4);
}

static uint8_t rgb565_to_gray8(uint16_t pixel)
{
    const unsigned r = ((pixel >> 11) & 0x1f) * 255U / 31U;
    const unsigned g = ((pixel >> 5) & 0x3f) * 255U / 63U;
    const unsigned b = (pixel & 0x1f) * 255U / 31U;
    return (uint8_t)((77U * r + 150U * g + 29U * b) >> 8);
}

static void flush_ssd1306(const uint16_t *src)
{
    // A fixed threshold keeps anti-aliased text edges clean on the SSD1306's one-bit panel.
    // Ordered dithering made small glyphs look speckled and uneven.
    memset(s_ssd1306_pixels, 0, sizeof(s_ssd1306_pixels));
    for (int y = 0; y < OLED_HEIGHT; ++y) {
        for (int x = 0; x < SSD1306_WIDTH; ++x) {
            if (rgb565_to_gray8(src[y * SSD1306_WIDTH + x]) >= 128) {
                s_ssd1306_pixels[(y / 8) * SSD1306_WIDTH + x] |= (uint8_t)(1U << (y & 7));
            }
        }
    }

    static const uint8_t address_range[] = {0x21, 0, SSD1306_WIDTH - 1, 0x22, 0, 7};
    if (send_commands(address_range, sizeof(address_range)) != ESP_OK) return;
    uint8_t packet[129];
    packet[0] = 0x40;
    for (size_t offset = 0; offset < sizeof(s_ssd1306_pixels); offset += sizeof(packet) - 1) {
        const size_t count = sizeof(s_ssd1306_pixels) - offset < sizeof(packet) - 1
                                 ? sizeof(s_ssd1306_pixels) - offset : sizeof(packet) - 1;
        memcpy(packet + 1, s_ssd1306_pixels + offset, count);
        if (i2c_master_transmit(s_i2c_device, packet, count + 1, 100) != ESP_OK) return;
    }
}

void chapter_display_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    // LVGL provides one complete RGB565 frame. SH1122 stores four gray bits per pixel; the SSD1306
    // backend thresholds the same frame into the controller's one-bit page layout.
    const uint16_t *src = (const uint16_t *)pixels;
    if (s_ssd1306) {
        if (s_i2c_device && s_enabled) flush_ssd1306(src);
    } else {
        for (size_t i = 0; i < SH1122_WIDTH * OLED_HEIGHT; i += 2) {
            const uint8_t left = rgb565_to_gray4(src[i]);
            const uint8_t right = rgb565_to_gray4(src[i + 1]);
            s_sh1122_pixels[i / 2] = (uint8_t)((left << 4) | right);
        }
        if (s_spi && s_enabled) {
            static const uint8_t address[] = {0x00, 0x10, 0xB0};
            if (spi_bytes(false, address, sizeof(address)) == ESP_OK) {
                spi_bytes(true, s_sh1122_pixels, sizeof(s_sh1122_pixels));
            }
        }
    }
    lv_display_flush_ready(display);
}
