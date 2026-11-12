/*
 * power_button.h — long-press the PRG button to power a LoRa node off
 *
 * Heltec V3/V4 have no power latch, so "off" is ESP32 deep sleep with the
 * SX1262 asleep and the OLED (Vext), LED and battery divider switched off.
 * Pressing the button again wakes the board, which reboots.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Call first thing in app_main: releases the pin holds set at power-off,
 * which otherwise persist after waking (OLED / radio would stay disabled). */
void power_button_boot(void);

void PowerButtonTask(void *pvParameters);

#ifdef __cplusplus
}
#endif
