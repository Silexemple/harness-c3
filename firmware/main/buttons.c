// Button polling: 10 ms poll, 30 ms debounce, short/long (≥600 ms). See
// buttons.h. Pins come exclusively from Kconfig (menu "Harness C3
// Configuration").

#include "buttons.h"
#include "sdkconfig.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "buttons";

#define POLL_MS     10
#define DEBOUNCE_MS 30
#define LONG_MS     600

// Debounce in poll ticks (30 ms / 10 ms = 3 identical reads to accept a level).
#define DEBOUNCE_TICKS (DEBOUNCE_MS / POLL_MS)
#define LONG_TICKS     (LONG_MS / POLL_MS)

typedef struct {
    gpio_num_t  gpio;
    btn_event_t short_event;
    btn_event_t long_event;
    int         level;        // accepted level, 1 = released (idle)
    int         candidate;    // level being debounced
    int         same_ticks;   // how long `candidate` has held
    int         held_ticks;   // ticks since an accepted press
    bool        long_fired;   // long event already sent for this press
} button_t;

static QueueHandle_t s_queue;

static void poll_button(button_t *b)
{
    const int raw = gpio_get_level(b->gpio);
    if (raw == b->candidate) {
        if (b->same_ticks < DEBOUNCE_TICKS) b->same_ticks++;
    } else {
        b->candidate = raw;
        b->same_ticks = 0;
    }
    if (b->same_ticks < DEBOUNCE_TICKS || b->candidate == b->level) {
        // Level unchanged (or still bouncing): only the long-press timer runs.
        if (b->level == 0 && !b->long_fired) {
            if (++b->held_ticks >= LONG_TICKS) {
                b->long_fired = true;
                xQueueSend(s_queue, &b->long_event, 0);
            }
        }
        return;
    }

    // A debounced edge.
    const int prev = b->level;
    b->level = b->candidate;
    if (b->level == 0) {           // pressed
        b->held_ticks = 0;
        b->long_fired = false;
    } else if (prev == 0 && !b->long_fired) {   // released before LONG_MS
        xQueueSend(s_queue, &b->short_event, 0);
    }
}

static void buttons_task(void *arg)
{
    (void)arg;
    static button_t btns[2] = {
        {
            .gpio = CONFIG_HARNESS_PIN_BTN_A,
            .short_event = BTN_EVENT_A_SHORT,
            .long_event = BTN_EVENT_A_LONG,
        },
        {
            .gpio = CONFIG_HARNESS_PIN_BTN_B,
            .short_event = BTN_EVENT_B_SHORT,
            .long_event = BTN_EVENT_B_LONG,
        },
    };
    for (int i = 0; i < 2; i++) {
        btns[i].level = 1;
        btns[i].candidate = 1;
    }
    for (;;) {
        poll_button(&btns[0]);
        poll_button(&btns[1]);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

bool buttons_init(QueueHandle_t queue)
{
    if (!queue) return false;
    s_queue = queue;

    const uint64_t mask = (1ULL << CONFIG_HARNESS_PIN_BTN_A) | (1ULL << CONFIG_HARNESS_PIN_BTN_B);
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,     // active-low buttons
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,       // polled, not interrupted
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "gpio config failed");
        return false;
    }

    if (xTaskCreate(buttons_task, "buttons", 2048, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    ESP_LOGI(TAG, "buttons on GPIO%d / GPIO%d (10 ms poll, 30 ms debounce, long >= %d ms)",
             CONFIG_HARNESS_PIN_BTN_A, CONFIG_HARNESS_PIN_BTN_B, LONG_MS);
    return true;
}
