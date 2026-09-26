// FreeRTOS shim for the host UI simulator: just enough of the API surface
// (queues, tasks, delays) over POSIX threads so the REAL ui.c compiles and
// runs unmodified on a laptop. 1 tick = 1 ms.
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef int BaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE   1
#define pdFALSE  0
#define pdPASS   1
#define pdFAIL   0

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

// Implemented in sim_freertos.c.
void vTaskDelay(TickType_t ticks);
