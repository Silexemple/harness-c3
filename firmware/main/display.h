// Display driver glue: GC9A01 round 240×240 SPI panel via esp_lcd, LVGL v9
// port, LEDC backlight.
//
// Pins and SPI clock come exclusively from Kconfig (menu "Harness C3
// Configuration", defaults in SPEC.md §2) — no pin number appears here.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 240

// Bring up the SPI bus, the GC9A01 panel, the LVGL display with its two DMA
// draw buffers (240×48 RGB565 each, ~45 KB total — SPEC.md §3 RAM budget),
// the esp_timer tick and the backlight. Returns false on any step's failure,
// leaving the firmware alive (the cable link still works headless).
bool display_init(void);

// The LVGL display lock. LVGL is single-threaded: every access from outside
// the UI task must hold this.
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);

// Backlight duty, 0–100 %. LEDC PWM on CONFIG_HARNESS_PIN_LCD_BL, active high.
void display_set_backlight(int percent);
