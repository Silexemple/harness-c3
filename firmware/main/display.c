// Display driver glue: GC9A01 round 240×240 SPI panel via esp_lcd, LVGL v9
// port, LEDC backlight. See display.h.

#include "display.h"
#include "sdkconfig.h"

#include <string.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "draw/sw/lv_draw_sw.h"   // lv_draw_sw_rgb565_swap()

static const char *TAG = "display";

// Two partial-frame draw buffers in DMA-capable internal RAM. 240×48 RGB565
// = 23 040 B each, ≈45 KB for the pair — the SPEC.md §3 RAM budget.
#define DRAW_BUF_HEIGHT 48
#define DRAW_BUF_BYTES  (DISPLAY_WIDTH * DRAW_BUF_HEIGHT * 2)

// LVGL tick period via esp_timer.
#define TICK_PERIOD_MS 2

#define LEDC_MODE     LEDC_LOW_SPEED_MODE
#define LEDC_TIMER    LEDC_TIMER_0
#define LEDC_CHANNEL  LEDC_CHANNEL_0
#define LEDC_FREQ_HZ  5000
#define LEDC_DUTY_RES LEDC_TIMER_10_BIT

static esp_lcd_panel_handle_t s_panel;
static lv_display_t          *s_disp;
static SemaphoreHandle_t      s_lvgl_lock;

// ── LVGL port ───────────────────────────────────────────────────────────────

// Called (in LVGL task context, via esp_lcd polling) when the DMA transfer of
// a flushed area has finished — only then may LVGL reuse the draw buffer.
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    // LVGL keeps RGB565 little-endian in memory; the panel wants MSB first.
    // LVGL v9 has no LV_COLOR_16_SWAP Kconfig — this is the v9 swap.
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)(w * h));
    // Queues the DMA transaction; lv_display_flush_ready is called from
    // on_color_trans_done when the transfer completes, so LVGL never reuses
    // a buffer the SPI master is still reading.
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(TICK_PERIOD_MS);
}

bool display_lock(uint32_t timeout_ms)
{
    if (!s_lvgl_lock) return false;
    return xSemaphoreTake(s_lvgl_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void display_unlock(void)
{
    if (s_lvgl_lock) xSemaphoreGive(s_lvgl_lock);
}

// ── backlight ───────────────────────────────────────────────────────────────

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
    const ledc_channel_config_t channel = {
        .gpio_num = CONFIG_HARNESS_PIN_LCD_BL,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "ledc channel");
    return ESP_OK;
}

void display_set_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const uint32_t duty = ((1 << LEDC_DUTY_RES) - 1) * (uint32_t)percent / 100;
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

// ── init ────────────────────────────────────────────────────────────────────

bool display_init(void)
{
    // Backlight first, at duty 0: the panel shows white noise while being
    // initialised, and that flash reads as a fault.
    if (backlight_init() != ESP_OK) {
        ESP_LOGE(TAG, "backlight init failed");
        return false;
    }
    display_set_backlight(0);

    // ── LVGL first: the panel IO's transaction-done callback below names the
    // LVGL display as its context, so the display must exist before the IO. ──
    lv_init();
    s_lvgl_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lvgl_lock) {
        ESP_LOGE(TAG, "no memory for the LVGL lock");
        return false;
    }

    uint8_t *buf1 = heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *buf2 = heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "no DMA RAM for draw buffers (%d B each)", DRAW_BUF_BYTES);
        heap_caps_free(buf1);
        heap_caps_free(buf2);
        return false;
    }

    s_disp = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, buf1, buf2, DRAW_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "SPI bus: sck=%d mosi=%d @ %d Hz",
             CONFIG_HARNESS_PIN_SPI_SCK, CONFIG_HARNESS_PIN_SPI_MOSI,
             CONFIG_HARNESS_LCD_SPI_FREQ_HZ);
    const spi_bus_config_t buscfg = {
        .sclk_io_num = CONFIG_HARNESS_PIN_SPI_SCK,
        .mosi_io_num = CONFIG_HARNESS_PIN_SPI_MOSI,
        .miso_io_num = -1,    // the panel is write-only
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DRAW_BUF_BYTES,
    };
    if (spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        return false;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = CONFIG_HARNESS_PIN_LCD_DC,
        .cs_gpio_num = CONFIG_HARNESS_PIN_LCD_CS,
        .pclk_hz = CONFIG_HARNESS_LCD_SPI_FREQ_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = s_disp,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io) != ESP_OK) {
        ESP_LOGE(TAG, "panel IO init failed");
        return false;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_HARNESS_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,   // GC9A01 round modules
        .bits_per_pixel = 16,                          // RGB565
    };
    if (esp_lcd_new_panel_gc9a01(io, &panel_config, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "GC9A01 panel init failed");
        return false;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    // These panels are inverted by default; without this every colour reads
    // as its complement.
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    const esp_timer_create_args_t tick_args = {
        .callback = tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick = NULL;
    if (esp_timer_create(&tick_args, &tick) != ESP_OK ||
        esp_timer_start_periodic(tick, TICK_PERIOD_MS * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL tick timer failed");
        return false;
    }

    display_set_backlight(100);
    ESP_LOGI(TAG, "GC9A01 %dx%d up, 2x%d B draw buffers", DISPLAY_WIDTH, DISPLAY_HEIGHT,
             DRAW_BUF_BYTES);
    return true;
}
