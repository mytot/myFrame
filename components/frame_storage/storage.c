#include "frame_storage/storage.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "wear_levelling.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "storage";

#define STORAGE_PARTITION_LABEL "storage"
#define STORAGE_MOUNT_POINT     "/apps"

static bool s_fs_ready;

esp_err_t frame_storage_mount_fs(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, STORAGE_PARTITION_LABEL);
    if (part == NULL) {
        ESP_LOGW(TAG, "no '%s' partition — app assets disabled", STORAGE_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    wl_handle_t wl;
    esp_err_t err = wl_mount(part, &wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wl_mount: %s", esp_err_to_name(err));
        return err;
    }

    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    err = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_MOUNT_POINT, wl, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fat mount: %s", esp_err_to_name(err));
        return err;
    }

    /* Apps live in their own subtree: /apps/<id>/. */
    char path[64];
    snprintf(path, sizeof(path), STORAGE_MOUNT_PATH "/apps");
    mkdir(path, 0755);

    s_fs_ready = true;
    ESP_LOGI(TAG, "asset filesystem mounted at %s (%u KB)",
             STORAGE_MOUNT_POINT, (unsigned)(part->size / 1024u));
    return ESP_OK;
}

bool frame_storage_fs_ready(void)
{
    return s_fs_ready;
}
