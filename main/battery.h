/*
 * battery.h — battery voltage readback for boards with a wired divider
 *
 * Usage (detector only):
 *   battery_init();
 *   uint32_t mv = battery_read_mv();   // 0 if unavailable / read failure
 *
 * All functions are no-ops on boards without BOARD_HAS_VBAT set; callers can
 * call unconditionally and check for 0 mV.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise ADC + control GPIO. Safe to call multiple times. */
void battery_init(void);

/*
 * Read battery voltage in millivolts.
 *
 * Enables the divider (drives PIN_VBAT_CTRL LOW), waits for the RC to settle,
 * averages several ADC samples, applies calibration + divider ratio, then
 * disables the divider again. Returns 0 if the board has no battery monitor
 * or the ADC is not ready.
 */
uint32_t battery_read_mv(void);

/* Disconnect the divider and hold the control pin through deep sleep. */
void battery_prepare_sleep(void);

#ifdef __cplusplus
}
#endif
