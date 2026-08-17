#include "sentry_device.h"

#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"

/*
 * Chip model is resolved at compile time from the IDF target macros rather than at
 * runtime from `esp_chip_info().model`. The enum gains a new constant with every new
 * silicon family, so a runtime switch either fails to build on older IDF or silently
 * reports "unknown" on newer chips; the CONFIG_IDF_TARGET_* macros have been stable
 * since IDF 4 and cost nothing at runtime.
 */
#if defined(CONFIG_IDF_TARGET_ESP32)
#define SENTRY_CHIP_MODEL "ESP32"
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
#define SENTRY_CHIP_MODEL "ESP32-S2"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define SENTRY_CHIP_MODEL "ESP32-S3"
#elif defined(CONFIG_IDF_TARGET_ESP32C2)
#define SENTRY_CHIP_MODEL "ESP32-C2"
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#define SENTRY_CHIP_MODEL "ESP32-C3"
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
#define SENTRY_CHIP_MODEL "ESP32-C5"
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
#define SENTRY_CHIP_MODEL "ESP32-C6"
#elif defined(CONFIG_IDF_TARGET_ESP32H2)
#define SENTRY_CHIP_MODEL "ESP32-H2"
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
#define SENTRY_CHIP_MODEL "ESP32-P4"
#else
#define SENTRY_CHIP_MODEL "ESP32-unknown"
#endif

static sentry_reset_reason_t map_reset_reason(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return SENTRY_RESET_POWERON;
    case ESP_RST_EXT:
        return SENTRY_RESET_EXTERNAL;
    case ESP_RST_SW:
        return SENTRY_RESET_SOFTWARE;
    case ESP_RST_PANIC:
        return SENTRY_RESET_PANIC;
    case ESP_RST_INT_WDT:
        return SENTRY_RESET_INT_WDT;
    case ESP_RST_TASK_WDT:
        return SENTRY_RESET_TASK_WDT;
    case ESP_RST_WDT:
        return SENTRY_RESET_WDT;
    case ESP_RST_DEEPSLEEP:
        return SENTRY_RESET_DEEPSLEEP;
    case ESP_RST_BROWNOUT:
        return SENTRY_RESET_BROWNOUT;
    case ESP_RST_SDIO:
        return SENTRY_RESET_SDIO;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
    case ESP_RST_USB:
        return SENTRY_RESET_USB;
    case ESP_RST_JTAG:
        return SENTRY_RESET_JTAG;
#endif
    default:
        return SENTRY_RESET_UNKNOWN;
    }
}

const char *sentry_reset_reason_name(sentry_reset_reason_t reason)
{
    switch (reason) {
    case SENTRY_RESET_POWERON:
        return "poweron";
    case SENTRY_RESET_EXTERNAL:
        return "external";
    case SENTRY_RESET_SOFTWARE:
        return "software";
    case SENTRY_RESET_PANIC:
        return "panic";
    case SENTRY_RESET_INT_WDT:
        return "int_wdt";
    case SENTRY_RESET_TASK_WDT:
        return "task_wdt";
    case SENTRY_RESET_WDT:
        return "wdt";
    case SENTRY_RESET_DEEPSLEEP:
        return "deepsleep";
    case SENTRY_RESET_BROWNOUT:
        return "brownout";
    case SENTRY_RESET_SDIO:
        return "sdio";
    case SENTRY_RESET_USB:
        return "usb";
    case SENTRY_RESET_JTAG:
        return "jtag";
    case SENTRY_RESET_UNKNOWN:
    default:
        return "unknown";
    }
}

bool sentry_reset_reason_is_crash(sentry_reset_reason_t reason)
{
    /* A software reset is *not* a crash: OTA updates and `ESP.restart()` land here and
     * would otherwise drown the real crashes. Brownout is included deliberately — it is
     * the single most common "my board randomly reboots" cause in the field. */
    switch (reason) {
    case SENTRY_RESET_PANIC:
    case SENTRY_RESET_INT_WDT:
    case SENTRY_RESET_TASK_WDT:
    case SENTRY_RESET_WDT:
    case SENTRY_RESET_BROWNOUT:
        return true;
    default:
        return false;
    }
}

void sentry_device_info_get(sentry_device_info_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    snprintf(out->chip_model, sizeof(out->chip_model), "%s", SENTRY_CHIP_MODEL);
    out->chip_revision = (uint16_t)chip.revision;
    out->cpu_cores = (uint8_t)chip.cores;

    snprintf(out->sdk_version, sizeof(out->sdk_version), "%s", esp_get_idf_version());

    /* The factory eFuse MAC is the only identifier that survives reflashing, NVS erase,
     * and a change of WiFi interface — so it is what the fleet is keyed on. Read from
     * eFuse directly rather than via WiFi so this works before (or without) any radio. */
    uint8_t mac[6] = { 0 };
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(out->device_id, sizeof(out->device_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
            mac[2], mac[3], mac[4], mac[5]);
    }

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        out->flash_size_bytes = flash_size;
    }

    out->total_heap_bytes = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    out->reset_reason = map_reset_reason(esp_reset_reason());
}

uint32_t sentry_device_free_heap(void) { return esp_get_free_heap_size(); }

uint32_t sentry_device_min_free_heap(void) { return esp_get_minimum_free_heap_size(); }

uint64_t sentry_device_uptime_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }
