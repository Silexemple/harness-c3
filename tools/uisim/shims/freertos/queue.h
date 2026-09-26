// Queue shim: a bounded ring buffer with a mutex. xQueueSend/Receive with
// a zero timeout is all ui.c uses (both directions: events and buttons).
#pragma once

#include "freertos/FreeRTOS.h"

typedef void *QueueHandle_t;

QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size);
BaseType_t    xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t    xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
