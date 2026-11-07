#pragma once

// Provisional signal map for the Chapter Player ESP32-S3 N16R8 port.
// GPIO numbers refer to the labels on the user's ESP32-S3 board, not header positions.

#define CHAPTER_PIN_OLED_SCLK 12
#define CHAPTER_PIN_OLED_MOSI 11
#define CHAPTER_PIN_OLED_CS   10
#define CHAPTER_PIN_OLED_DC    9
#define CHAPTER_PIN_OLED_RST   8

// The SSD1306 I2C option reuses the two display bus GPIOs as SDA/SCL.
#define CHAPTER_PIN_OLED_I2C_SDA CHAPTER_PIN_OLED_MOSI
#define CHAPTER_PIN_OLED_I2C_SCL CHAPTER_PIN_OLED_SCLK

#define CHAPTER_PIN_SELECT_A  4
#define CHAPTER_PIN_SELECT_B  5
#define CHAPTER_PIN_SELECT_SW 6

#define CHAPTER_PIN_VOLUME_A  7
#define CHAPTER_PIN_VOLUME_B 15
#define CHAPTER_PIN_VOLUME_SW 16

#define CHAPTER_PIN_I2S_BCLK  17
#define CHAPTER_PIN_I2S_LRCLK 18
#define CHAPTER_PIN_I2S_DOUT  21
#define CHAPTER_PIN_AMP_SD    13
