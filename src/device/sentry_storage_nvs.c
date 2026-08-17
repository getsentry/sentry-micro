#include "sentry_storage_nvs.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

/* Keys are capped at 15 characters by NVS, so slots get a short prefix. */
#define SENTRY_NVS_NAMESPACE "sentry"
#define SENTRY_NVS_META_KEY "sm_meta"
#define SENTRY_NVS_MAX_SLOTS 64

/** Guards against reading back an uninitialised or foreign blob as ring indices. */
#define SENTRY_NVS_META_MAGIC 0x534D4231u /* "SMB1" */

typedef struct {
    uint32_t magic;
    uint32_t head;
    uint32_t tail;
    uint32_t dropped;
} sentry_nvs_meta_t;

static sentry_storage_t g_storage;

static void slot_key(uint32_t index, char *out, size_t cap)
{
    snprintf(out, cap, "sm_e%u", (unsigned)index);
}

/**
 * Open the namespace for one operation.
 *
 * Opened and closed per call rather than held: a long-lived handle would have to survive
 * an OTA and every failure path in between, and NVS opens are cheap next to the flash
 * write they accompany.
 */
static bool open_handle(nvs_handle_t *handle)
{
    /* The Arduino core already calls nvs_flash_init() during startup, and ESP-IDF apps are
     * expected to. If it has not happened, opening fails and buffering stays off — which is
     * the correct outcome, because the alternative is erasing flash we do not own. */
    return nvs_open(SENTRY_NVS_NAMESPACE, NVS_READWRITE, handle) == ESP_OK;
}

static bool nvs_write(void *ctx, uint32_t index, const uint8_t *data, size_t len)
{
    (void)ctx;
    nvs_handle_t handle;
    if (!open_handle(&handle)) {
        return false;
    }

    char key[16];
    slot_key(index, key, sizeof(key));

    esp_err_t err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        /* Without the commit the write sits in RAM and is lost on the reset we are very
         * probably about to take — which is precisely the case this buffer exists for. */
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

static bool nvs_read(void *ctx, uint32_t index, uint8_t *out, size_t cap, size_t *out_len)
{
    (void)ctx;
    nvs_handle_t handle;
    if (!open_handle(&handle)) {
        return false;
    }

    char key[16];
    slot_key(index, key, sizeof(key));

    size_t length = cap;
    esp_err_t err = nvs_get_blob(handle, key, out, &length);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }
    *out_len = length;
    return true;
}

static bool nvs_erase(void *ctx, uint32_t index)
{
    (void)ctx;
    nvs_handle_t handle;
    if (!open_handle(&handle)) {
        return false;
    }

    char key[16];
    slot_key(index, key, sizeof(key));

    esp_err_t err = nvs_erase_key(handle, key);
    /* Erasing something that is not there is the desired end state, not a failure. */
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

static bool nvs_load_meta(void *ctx, uint32_t *head, uint32_t *tail, uint32_t *dropped)
{
    (void)ctx;
    nvs_handle_t handle;
    if (!open_handle(&handle)) {
        return false;
    }

    sentry_nvs_meta_t meta;
    size_t length = sizeof(meta);
    esp_err_t err = nvs_get_blob(handle, SENTRY_NVS_META_KEY, &meta, &length);
    nvs_close(handle);

    if (err != ESP_OK || length != sizeof(meta) || meta.magic != SENTRY_NVS_META_MAGIC) {
        /* No metadata yet, or something else wrote this key. Either way the buffer starts
         * empty; the ring policy also range-checks whatever it is given. */
        return false;
    }
    *head = meta.head;
    *tail = meta.tail;
    *dropped = meta.dropped;
    return true;
}

static bool nvs_save_meta(void *ctx, uint32_t head, uint32_t tail, uint32_t dropped)
{
    (void)ctx;
    nvs_handle_t handle;
    if (!open_handle(&handle)) {
        return false;
    }

    sentry_nvs_meta_t meta;
    meta.magic = SENTRY_NVS_META_MAGIC;
    meta.head = head;
    meta.tail = tail;
    meta.dropped = dropped;

    esp_err_t err = nvs_set_blob(handle, SENTRY_NVS_META_KEY, &meta, sizeof(meta));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

const sentry_storage_t *sentry_storage_nvs(uint32_t slot_count)
{
    if (slot_count == 0 || slot_count > SENTRY_NVS_MAX_SLOTS) {
        return NULL;
    }

    /* Prove NVS is actually reachable now rather than discovering it on the crash path. */
    nvs_handle_t handle;
    if (!open_handle(&handle)) {
        return NULL;
    }
    nvs_close(handle);

    g_storage.write = nvs_write;
    g_storage.read = nvs_read;
    g_storage.erase = nvs_erase;
    g_storage.load_meta = nvs_load_meta;
    g_storage.save_meta = nvs_save_meta;
    g_storage.ctx = NULL;
    g_storage.slot_count = slot_count;
    return &g_storage;
}
