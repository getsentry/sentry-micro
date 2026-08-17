#include "sentry_coredump_device.h"

#include <stdio.h>
#include <string.h>

#include "esp_core_dump.h"

/*
 * The whole path depends on the panic handler having somewhere to write. Both conditions
 * hold by default in the Arduino core (verified in its sdkconfig.h), but a custom ESP-IDF
 * build can switch them off, and silently producing no backtraces would be worse than not
 * compiling. Degrade to "no core dump available" with a reason attached instead.
 */
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
#    define SENTRY_COREDUMP_SUPPORTED 1
#else
#    define SENTRY_COREDUMP_SUPPORTED 0
#endif

#if SENTRY_COREDUMP_SUPPORTED

/**
 * Translate an Xtensa EXCCAUSE into the name the panic handler prints.
 *
 * These are the strings a maker has already seen on their serial console next to "Guru
 * Meditation Error", so using the same vocabulary means the Sentry issue title matches what
 * they searched the forums for. A raw cause number would not.
 */
static const char *exception_name(uint32_t cause)
{
    switch (cause) {
    case 0:
        return "IllegalInstruction";
    case 1:
        return "Syscall";
    case 2:
        return "InstructionFetchError";
    case 3:
        return "LoadStoreError";
    case 4:
        return "Level1Interrupt";
    case 5:
        return "Alloca";
    case 6:
        return "IntegerDivideByZero";
    case 8:
        return "Privileged";
    case 9:
        return "LoadStoreAlignment";
    case 12:
        return "InstrPIFDataError";
    case 13:
        return "LoadStorePIFDataError";
    case 14:
        return "InstrPIFAddrError";
    case 15:
        return "LoadStorePIFAddrError";
    case 16:
        return "InstTLBMiss";
    case 17:
        return "InstTLBMultiHit";
    case 18:
        return "InstFetchPrivilege";
    case 20:
        return "InstFetchProhibited";
    case 24:
        return "LoadStoreTLBMiss";
    case 25:
        return "LoadStoreTLBMultiHit";
    case 26:
        return "LoadStorePrivilege";
    case 28:
        return "LoadProhibited";
    case 29:
        return "StoreProhibited";
    default:
        return "";
    }
}

#endif /* SENTRY_COREDUMP_SUPPORTED */

bool sentry_coredump_is_supported(void) { return SENTRY_COREDUMP_SUPPORTED; }

bool sentry_coredump_available(void)
{
#if SENTRY_COREDUMP_SUPPORTED
    /* Verifies the stored image's checksum, so a dump interrupted by the reset it was
     * reporting does not come back as plausible-looking garbage. */
    return esp_core_dump_image_check() == ESP_OK;
#else
    return false;
#endif
}

bool sentry_coredump_read(sentry_coredump_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

#if SENTRY_COREDUMP_SUPPORTED
    if (esp_core_dump_image_check() != ESP_OK) {
        return false;
    }

    /*
     * ~2.5 KB. Deliberately on the caller's stack rather than the heap: this runs early in
     * boot right after a crash, which is exactly when the heap is least trustworthy. The
     * cost is that the calling task needs the headroom — documented on the declaration.
     */
    esp_core_dump_summary_t summary;
    if (esp_core_dump_get_summary(&summary) != ESP_OK) {
        return false;
    }

    snprintf(out->task_name, sizeof(out->task_name), "%s", summary.exc_task);
    out->exception_pc = summary.exc_pc;
    snprintf(out->exception_type, sizeof(out->exception_type), "%s",
        exception_name(summary.ex_info.exc_cause));
    out->exception_addr = summary.ex_info.exc_vaddr;
    /* `exc_vaddr` only holds a faulting data address for load/store exceptions; for the
     * rest it is stale. Reporting it regardless would attach a confident-looking address to
     * an IllegalInstruction that has nothing to do with it. */
    switch (summary.ex_info.exc_cause) {
    case 3: /* LoadStoreError */
    case 9: /* LoadStoreAlignment */
    case 24: /* LoadStoreTLBMiss */
    case 25: /* LoadStoreTLBMultiHit */
    case 26: /* LoadStorePrivilege */
    case 28: /* LoadProhibited */
    case 29: /* StoreProhibited */
        out->exception_addr_valid = true;
        break;
    default:
        out->exception_addr_valid = false;
        break;
    }

    /* ESP-IDF stores the ELF hash as a printable string already. */
    snprintf(out->app_elf_sha256, sizeof(out->app_elf_sha256), "%s",
        (const char *)summary.app_elf_sha256);

    uint32_t depth = summary.exc_bt_info.depth;
    if (depth > SENTRY_MICRO_MAX_FRAMES) {
        depth = SENTRY_MICRO_MAX_FRAMES;
    }
    for (uint32_t i = 0; i < depth; i++) {
        out->frames[i] = summary.exc_bt_info.bt[i];
    }
    out->frame_count = depth;
    out->truncated = summary.exc_bt_info.corrupted;

    out->available = true;
    return true;
#else
    return false;
#endif
}

bool sentry_coredump_erase(void)
{
#if SENTRY_COREDUMP_SUPPORTED
    /* Erasing a partition that holds nothing is the desired end state, not a failure. */
    esp_err_t err = esp_core_dump_image_erase();
    return err == ESP_OK || err == ESP_ERR_NOT_FOUND;
#else
    return false;
#endif
}
