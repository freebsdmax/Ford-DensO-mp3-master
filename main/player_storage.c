#include "player_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static const char *TAG = "STORAGE";
static const char *BASE = "/storage";
static bool s_ready = false;
static wl_handle_t s_wl = WL_INVALID_HANDLE;

bool player_storage_sanitize_name(const char *name, char *dst, size_t cap)
{
    if (!name || !dst || cap < 6) return false;
    size_t o = 0;
    for (const char *p = name; *p && o + 1 < cap; ++p) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
        dst[o++] = c;
    }
    dst[o] = '\0';
    if (o < 5) return false;
    if (strstr(dst, "..")) return false;
    if (dst[0] == '.') return false;
    return strstr(dst, ".bin") != NULL;
}

esp_err_t player_storage_init(void)
{
    if (s_ready) return ESP_OK;

    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(BASE, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    ESP_LOGI(TAG, "FAT mounted at %s", BASE);
    return ESP_OK;
}

bool player_storage_ready(void)
{
    return s_ready;
}

static void full_path(const char *name, char *dst, size_t cap)
{
    snprintf(dst, cap, "%s/%s", BASE, name);
}

bool player_storage_file_size(const char *name, size_t *size)
{
    if (!s_ready || !name) return false;
    char path[64];
    full_path(name, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (size) *size = (size_t)st.st_size;
    return true;
}

esp_err_t player_storage_write_file(const char *name, const uint8_t *data, size_t len)
{
    if (!s_ready) {
        esp_err_t err = player_storage_init();
        if (err != ESP_OK) return err;
    }
    if (!name || !data || len == 0) return ESP_ERR_INVALID_ARG;

    char path[64];
    full_path(name, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s for writing", path);
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "short write %u/%u", (unsigned)written, (unsigned)len);
        remove(path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved %s (%u bytes)", path, (unsigned)len);
    return ESP_OK;
}

esp_err_t player_storage_read_file(const char *name, uint8_t **out, size_t *out_len)
{
    if (!s_ready || !name || !out || !out_len) return ESP_ERR_INVALID_STATE;
    *out = NULL;
    *out_len = 0;

    char path[64];
    full_path(name, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGW(TAG, "%s not found", path);
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    size_t len = (size_t)st.st_size;
    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = heap_caps_malloc(len, MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) {
        free(buf);
        return ESP_FAIL;
    }
    *out = buf;
    *out_len = len;
    return ESP_OK;
}

esp_err_t player_storage_delete_file(const char *name)
{
    if (!s_ready || !name) return ESP_ERR_INVALID_STATE;
    char path[64];
    full_path(name, path, sizeof(path));
    if (remove(path) != 0) {
        ESP_LOGW(TAG, "cannot delete %s", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "deleted %s", path);
    return ESP_OK;
}
