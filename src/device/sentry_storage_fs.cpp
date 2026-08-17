#include "sentry_storage_fs.hpp"

#if defined(ARDUINO)

#    include <stdio.h>
#    include <string.h>

namespace sentry {
namespace {

/** Guards against reading a foreign or half-written file back as ring indices. */
const uint32_t META_MAGIC = 0x534D4231u; /* "SMB1" */

struct MetaFile {
    uint32_t magic;
    uint32_t head;
    uint32_t tail;
    uint32_t dropped;
};

struct FsContext {
    fs::FS *filesystem;
    char dir[32];
};

FsContext g_context;
sentry_storage_t g_storage;

void slot_path(uint32_t index, char *out, size_t cap)
{
    snprintf(out, cap, "%s/e%u", g_context.dir, (unsigned)index);
}

void meta_path(char *out, size_t cap) { snprintf(out, cap, "%s/meta", g_context.dir); }

bool fs_write(void *ctx, uint32_t index, const uint8_t *data, size_t len)
{
    (void)ctx;
    char path[64];
    slot_path(index, path, sizeof(path));

    File file = g_context.filesystem->open(path, FILE_WRITE);
    if (!file) {
        return false;
    }
    size_t written = file.write(data, len);
    /* Close before judging: LittleFS defers the metadata update to close, so a short write
     * can only be detected reliably once the file is actually committed. */
    file.close();
    if (written != len) {
        /* Leave nothing behind that a later read could mistake for a whole envelope. */
        g_context.filesystem->remove(path);
        return false;
    }
    return true;
}

bool fs_read(void *ctx, uint32_t index, uint8_t *out, size_t cap, size_t *out_len)
{
    (void)ctx;
    char path[64];
    slot_path(index, path, sizeof(path));

    File file = g_context.filesystem->open(path, FILE_READ);
    if (!file) {
        return false;
    }
    size_t size = file.size();
    if (size == 0 || size > cap) {
        /* Too large for the caller's scratch buffer. Reported as a failure so the flush
         * path discards it rather than wedging the queue behind an unreadable entry. */
        file.close();
        return false;
    }
    size_t read = file.read(out, size);
    file.close();
    if (read != size) {
        return false;
    }
    *out_len = size;
    return true;
}

bool fs_erase(void *ctx, uint32_t index)
{
    (void)ctx;
    char path[64];
    slot_path(index, path, sizeof(path));
    if (!g_context.filesystem->exists(path)) {
        /* Already gone is the desired end state, not a failure. */
        return true;
    }
    return g_context.filesystem->remove(path);
}

bool fs_load_meta(void *ctx, uint32_t *head, uint32_t *tail, uint32_t *dropped)
{
    (void)ctx;
    char path[64];
    meta_path(path, sizeof(path));

    File file = g_context.filesystem->open(path, FILE_READ);
    if (!file) {
        return false;
    }
    MetaFile meta;
    size_t read = file.read((uint8_t *)&meta, sizeof(meta));
    file.close();

    if (read != sizeof(meta) || meta.magic != META_MAGIC) {
        return false;
    }
    *head = meta.head;
    *tail = meta.tail;
    *dropped = meta.dropped;
    return true;
}

bool fs_save_meta(void *ctx, uint32_t head, uint32_t tail, uint32_t dropped)
{
    (void)ctx;
    char path[64];
    meta_path(path, sizeof(path));

    File file = g_context.filesystem->open(path, FILE_WRITE);
    if (!file) {
        return false;
    }
    MetaFile meta = { META_MAGIC, head, tail, dropped };
    size_t written = file.write((const uint8_t *)&meta, sizeof(meta));
    file.close();
    return written == sizeof(meta);
}

} // namespace

const sentry_storage_t *storage_fs(fs::FS &filesystem, uint32_t slot_count, const char *dir)
{
    if (slot_count == 0 || !dir || dir[0] != '/') {
        return NULL;
    }

    g_context.filesystem = &filesystem;
    snprintf(g_context.dir, sizeof(g_context.dir), "%s", dir);

    /* Create the directory if it is missing, but never mount or format — the application
     * owns the filesystem, and reformatting someone's data to report a crash is not a
     * trade this SDK gets to make. A failure here is how an unmounted filesystem shows up. */
    if (!filesystem.exists(g_context.dir) && !filesystem.mkdir(g_context.dir)) {
        return NULL;
    }

    g_storage.write = fs_write;
    g_storage.read = fs_read;
    g_storage.erase = fs_erase;
    g_storage.load_meta = fs_load_meta;
    g_storage.save_meta = fs_save_meta;
    g_storage.ctx = NULL;
    g_storage.slot_count = slot_count;
    return &g_storage;
}

} // namespace sentry

#endif /* ARDUINO */
