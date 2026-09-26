// UI: LVGL v9 screens for the 240×240 round panel (SPEC.md §6).
//
//   boot → not-connected → home (agent carousel, one agent per page,
//   status ring colored by state) → question screen; plus a notification
//   badge/overlay and toasts.
//
// Round-panel rule: all critical text stays inside the inscribed square
// (~170×170 centered).
//
// ui_init() creates the screens and the LVGL task, and registers this UI as
// the cable_client event sink. Buttons post to ui_button_queue() (buttons.c).
#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Requires display_init() to have succeeded. `fw_version` is printed on the
// boot screen (the running image's version, e.g. "0.1.0-c3").
bool ui_init(const char *fw_version);

// The queue buttons_init() posts btn_event_t to.
QueueHandle_t ui_button_queue(void);
