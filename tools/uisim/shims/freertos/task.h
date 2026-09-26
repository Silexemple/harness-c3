// Task shim: xTaskCreate spawns a detached pthread running the function.
#pragma once

#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, uint32_t prio, void *handle);
