/*
 * config_console.h — Interactive serial console for runtime configuration
 *
 * Provides show / set / reboot commands over UART0.
 * Config values are persisted to NVS; a reboot is required to apply them.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the serial console REPL and register commands.
 * Call once from app_main() after nvs_flash_init().
 */
void config_console_init(void);

#ifdef __cplusplus
}
#endif
