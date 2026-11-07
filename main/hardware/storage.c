// Wear-levelled FAT cache on the ESP32-S3's onboard flash. There is no microSD socket in Chapter.

#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static const char *TAG = "storage";
static wl_handle_t s_wl = WL_INVALID_HANDLE;

esp_err_t storage_init(void)
{
    const esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/storage", "storage", &config, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not mount onboard library cache (%s)", esp_err_to_name(err));
        s_wl = WL_INVALID_HANDLE;
        return err;
    }
    storage_mkdirs(STORAGE_ROOT);
    uint64_t free_bytes = 0, total_bytes = 0;
    storage_space(&free_bytes, &total_bytes);
    ESP_LOGI(TAG, "onboard cache mounted: %llu MB free of %llu MB",
             (unsigned long long)(free_bytes >> 20), (unsigned long long)(total_bytes >> 20));
    return ESP_OK;
}

bool storage_ready(void)
{
    return s_wl != WL_INVALID_HANDLE;
}

void storage_space(uint64_t *free_bytes, uint64_t *total_bytes)
{
    *free_bytes = *total_bytes = 0;
    if (storage_ready()) esp_vfs_fat_info("/storage", total_bytes, free_bytes);
}

// cJSON library data can live in PSRAM; file I/O uses ordinary stdio so no DMA restrictions apply.
char *storage_read_file(const char *path, size_t *len)
{
    if (len) *len = 0;
    if (!storage_ready()) return NULL;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    char *buffer = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) buffer = malloc((size_t)size + 1);
    if (!buffer) { fclose(file); return NULL; }
    const size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) { free(buffer); return NULL; }
    buffer[size] = 0;
    if (len) *len = (size_t)size;
    return buffer;
}

esp_err_t storage_write_file(const char *path, const void *data, size_t len)
{
    if (!storage_ready()) return ESP_ERR_INVALID_STATE;
    char temporary[192];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *file = fopen(temporary, "wb");
    if (!file) return ESP_FAIL;
    bool ok = fwrite(data, 1, len, file) == len;
    ok &= fflush(file) == 0;
    ok &= fclose(file) == 0;
    if (ok) {
        unlink(path);
        ok = rename(temporary, path) == 0;
    }
    if (!ok) {
        ESP_LOGW(TAG, "writing %s failed (errno %d)", path, errno);
        unlink(temporary);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t storage_mkdirs(const char *path)
{
    char current[192];
    if (strlcpy(current, path, sizeof(current)) >= sizeof(current)) return ESP_ERR_INVALID_SIZE;
    for (char *cursor = current + 1; *cursor; ++cursor) {
        if (*cursor != '/') continue;
        *cursor = 0;
        if (mkdir(current, 0775) != 0 && errno != EEXIST) { *cursor = '/'; return ESP_FAIL; }
        *cursor = '/';
    }
    return (mkdir(current, 0775) == 0 || errno == EEXIST) ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    if (!directory) return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    struct dirent *entry;
    char child[256];
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child)) continue;
        if (entry->d_type == DT_DIR) storage_remove_tree(child);
        else unlink(child);
    }
    closedir(directory);
    return rmdir(path) == 0 ? ESP_OK : ESP_FAIL;
}
