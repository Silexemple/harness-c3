// Shim implementations: queues, tasks, delays, display lock, buzzer.
// See README.md — this file replaces the ESP-IDF platform on a laptop.

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "buzzer.h"

void vTaskDelay(TickType_t ticks)
{
    usleep((useconds_t)ticks * 1000);
}

// ── queues ──────────────────────────────────────────────────────────────────

typedef struct {
    pthread_mutex_t mu;
    uint8_t *buf;
    uint32_t item_size, length, head, tail, count;
} sim_queue_t;

QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size)
{
    sim_queue_t *q = calloc(1, sizeof(*q));
    q->buf = malloc(length * item_size);
    q->item_size = item_size;
    q->length = length;
    pthread_mutex_init(&q->mu, NULL);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t qh, const void *item, TickType_t wait)
{
    (void)wait;   // ui.c always sends with 0
    sim_queue_t *q = qh;
    pthread_mutex_lock(&q->mu);
    if (q->count == q->length) { pthread_mutex_unlock(&q->mu); return pdFALSE; }
    memcpy(q->buf + q->tail * q->item_size, item, q->item_size);
    q->tail = (q->tail + 1) % q->length;
    q->count++;
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t qh, void *item, TickType_t wait)
{
    (void)wait;
    sim_queue_t *q = qh;
    pthread_mutex_lock(&q->mu);
    if (q->count == 0) { pthread_mutex_unlock(&q->mu); return pdFALSE; }
    memcpy(item, q->buf + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->length;
    q->count--;
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}

// ── tasks ───────────────────────────────────────────────────────────────────

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, uint32_t prio, void *handle)
{
    (void)name; (void)stack; (void)prio; (void)handle;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &attr, (void *(*)(void *))fn, arg);
    pthread_attr_destroy(&attr);
    return rc == 0 ? pdPASS : pdFAIL;
}

// ── display lock (display.c equivalent) ─────────────────────────────────────

static pthread_mutex_t s_lvgl_mu = PTHREAD_MUTEX_INITIALIZER;

bool display_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    pthread_mutex_lock(&s_lvgl_mu);
    return true;
}

void display_unlock(void)
{
    pthread_mutex_unlock(&s_lvgl_mu);
}

void display_set_backlight(int percent) { (void)percent; }

// ── buzzer: audible on paper ────────────────────────────────────────────────

void buzzer_beep(buzzer_pattern_t pattern)
{
    fprintf(stderr, "[buzzer] %s\n",
            pattern == BUZZER_BEEP_DONE ? "done" :
            pattern == BUZZER_BEEP_QUESTION ? "question" : "error");
}
