idf_component_register(
    SRCS
        "app_main.c"
        "cable_frame.c"
        "cable_link.c"
        "cable_client.c"
        "display.c"
        "ui.c"
        "buttons.c"
        "buzzer.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_usb_serial_jtag   # USB-Serial-JTAG transport
        esp_driver_gpio              # buttons
        esp_driver_spi               # GC9A01 panel bus
        esp_driver_ledc              # backlight + buzzer PWM
        esp_lcd                      # panel abstraction
        esp_timer                    # LVGL tick
        nvs_flash                    # boot counter / future settings
        app_update                   # esp_ota_mark_app_valid_cancel_rollback
        json                         # cJSON
        lvgl                         # UI
)
