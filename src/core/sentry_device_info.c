#include "sentry_device_info.h"

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
