/* LVGL v9 configuration for the host UI simulator.
 *
 * Deliberately mirrors the device's trimmed build (firmware/sdkconfig.defaults):
 * 16-bit RGB565, the 32 KB built-in LVGL pool, and only the three Montserrat
 * sizes the firmware uses. Everything else stays at lv_conf_internal.h
 * defaults (which LVGL v9 supplies for any symbol not defined here).
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE

/* Memory: built-in allocator with the same 32 KB pool as the device. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (32U * 1024U)
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN

/* Fonts: exactly the device's set — Montserrat 14/20/28, nothing else. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1

#endif /* LV_CONF_H */
