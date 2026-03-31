/*
 * main.c — superseded by the dual-core refactor on branch feature/lora-standalone
 *
 * Original monolithic app_main() has been split into:
 *   main.cpp      — entry point, queue + task creation
 *   audio_task.c  — I2S + FFT harmonic engine (Core 1)
 *   lora_task.cpp — SX1262 LoRa transmitter via RadioLib (Core 0)
 *
 * This file is intentionally empty and excluded from CMakeLists.txt SRCS.
 */
