// Two push buttons, active-low on internal pull-ups (SPEC.md §2).
//
// 10 ms poll, 30 ms debounce, short/long (≥600 ms) events posted to a queue
// the UI task drains. BTN_A short = next/scroll, long = confirm/yes;
// BTN_B short = back, on a question = no/cancel.
#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    BTN_EVENT_A_SHORT = 0,
    BTN_EVENT_A_LONG,
    BTN_EVENT_B_SHORT,
    BTN_EVENT_B_LONG,
} btn_event_t;

// Install the GPIO inputs and start the polling task. `queue` receives one
// btn_event_t per completed press; it is owned by the caller (the UI).
// Returns false when the task could not be created.
bool buttons_init(QueueHandle_t queue);
