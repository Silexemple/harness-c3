// Harness C3 — entry point. Init order is fixed by SPEC.md §3:
//
//   NVS → display+LVGL → UI (boot screen, then "Not connected") → buttons →
//   buzzer → cable_link → cable_client (starts the hello cadence) →
//   esp_ota_mark_app_valid_cancel_rollback() once all init succeeded.
//
// A failure in any single subsystem is logged and survived where possible:
// a dial that shows "Not connected" is diagnosable from across the room,
// a boot loop is not.

#include <stdio.h>
#include <string.h>

#include "buttons.h"
#include "sdkconfig.h"
#include "buzzer.h"
#include "cable_client.h"
#include "cable_link.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ui.h"

static const char *TAG = "app";

// cable_client's clock (the seam that lets host tests drive the session
// machine with a fake one).
uint32_t cable_platform_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void app_main(void)
{
    // ── NVS (required by app_update/OTA plumbing) ──
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // ── display + LVGL, then UI ──
    const bool display_ok = display_init();
    if (!display_ok) ESP_LOGE(TAG, "display init failed — running headless");

    // Identity for `hello`: version from the RUNNING image (never a constant
    // — the daemon's offer decision compares this string, PROTOCOL.md §10.10),
    // suffixed "-c3" for this port; MAC = WiFi STA MAC, uppercase,
    // colon-separated (also the device's USB serial number).
    char fw[40];
    snprintf(fw, sizeof(fw), "%s-c3", esp_app_get_description()->version);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[24];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cable_client_set_identity(fw, CABLE_HW_NAME, mac_str);

    if (display_ok) ui_init(fw);

    // ── buttons + buzzer ──
    if (ui_button_queue()) buttons_init(ui_button_queue());
    buzzer_init();   // no-op when CONFIG_HARNESS_PIN_BUZZER == -1

    // ── cable link, then client (starts the hello cadence) ──
    cable_client_init();
    if (!cable_link_start(cable_client_handle_frame, NULL)) {
        ESP_LOGE(TAG, "cable link failed to start — no connection to the daemon");
    }
    cable_client_start();

    // ── all init done: this image is good, cancel any pending rollback ──
    err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "OTA rollback cancel: %s (running partition is not pending verify)",
                 esp_err_to_name(err));
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "harness-c3 %s up (proto %d)", fw, CABLE_PROTO_VERSION);
}
