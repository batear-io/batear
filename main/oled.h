/*
 * oled.h — Minimal SSD1306 128x64 I2C OLED driver for Heltec V3
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* flip = false: default orientation; true: rotated 180° from it
 * (the orientation used before v3.0). */
esp_err_t oled_init(int sda_gpio, int scl_gpio, int rst_gpio, bool flip);
void      oled_clear(void);
void      oled_print(int col, int row, const char *text);
void      oled_flush(void);

#ifdef __cplusplus
}
#endif
