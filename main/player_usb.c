#include "player_usb.h"
#include "player_nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "esp_vfs_fat.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG_USB = "Player";

msc_dev_entry_t *msc_devices[MAX_MSC_DEVICES] = {0};
extern bool g_mode_external;
volatile bool g_usb_reinit_request = false;

esp_err_t allocate_new_msc_device(const app_message_t *msg, int *out_slot, bool *is_new)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i] && msc_devices[i]->usb_addr == msg->data.new_dev_address) {
            *out_slot = i;
            if (is_new) *is_new = false;
            return ESP_OK;
        }
    }
    int slot = find_free_slot();
    if (slot < 0) {
        ESP_LOGW(TAG_USB, "⚠️ Немає вільних слотів для нового пристрою MSC (макс. %d)", MAX_MSC_DEVICES);
        return ESP_ERR_NOT_FOUND;
    }

    msc_devices[slot] = calloc(1, sizeof(msc_dev_entry_t));
    if (!msc_devices[slot]) {
        ESP_LOGE(TAG_USB, "❌ Не вдалося виділити пам'ять для запису пристрою MSC");
        return ESP_ERR_NO_MEM;
    }
    
    esp_err_t err = msc_host_install_device(msg->data.new_dev_address, &msc_devices[slot]->msc_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_USB, "❌ msc_host_install_device не вдалося: %s", esp_err_to_name(err));
        free(msc_devices[slot]);
        msc_devices[slot] = NULL;
        return err;
    }

    msc_devices[slot]->usb_addr = msg->data.new_dev_address;

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };

    char mount_path[16];
    snprintf(mount_path, sizeof(mount_path), MNT_PATH "%d", slot);

    err = msc_host_vfs_register(msc_devices[slot]->msc_device, mount_path, &mount_config, &msc_devices[slot]->vfs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_USB, "❌ msc_host_vfs_register не вдалося: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(msc_host_uninstall_device(msc_devices[slot]->msc_device));
        free(msc_devices[slot]);
        msc_devices[slot] = NULL;
        return err;
    }

    *out_slot = slot;
    if (is_new) *is_new = true;
    return ESP_OK;
}

int find_slot_by_handle(msc_host_device_handle_t handle)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i] && msc_devices[i]->msc_device == handle) {
            return i;
        }
    }
    return -1;
}

void free_msc_device(int slot)
{
    if (slot < 0 || slot >= MAX_MSC_DEVICES || !msc_devices[slot]) {
        ESP_LOGE(TAG_USB, "❌ Невірний індекс слота для звільнення пристрою MSC");
        return;
    }

    if (msc_devices[slot]->vfs_handle) {
        ESP_ERROR_CHECK(msc_host_vfs_unregister(msc_devices[slot]->vfs_handle));
    }
    if (msc_devices[slot]->msc_device) {
        ESP_ERROR_CHECK(msc_host_uninstall_device(msc_devices[slot]->msc_device));
    }

    free(msc_devices[slot]);
    msc_devices[slot] = NULL;
}

void free_all_msc_devices(void)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i]) {
            free_msc_device(i);
        }
    }
}

void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    if (event->event == MSC_DEVICE_CONNECTED) {
        ESP_LOGI(TAG_USB, "💾 Пристрій MSC підключено (usb_addr=%d)", event->device.address);
        app_message_t message = {
            .id = APP_DEVICE_CONNECTED,
            .data.new_dev_address = event->device.address,
        };
        xQueueSend(app_queue, &message, portMAX_DELAY);
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        int usb_addr = find_usb_addr_by_handle(event->device.handle);
        if (usb_addr >= 0) {
            ESP_LOGI(TAG_USB, "📤 Пристрій MSC відключено (usb_addr=%d)", usb_addr);
        } else {
            ESP_LOGW(TAG_USB, "⚠️ Пристрій MSC відключено, але не вдалося отримати USB адресу");
        }
        app_message_t message = {
            .id = APP_DEVICE_DISCONNECTED,
            .data.device_handle = event->device.handle,
        };
        xQueueSend(app_queue, &message, portMAX_DELAY);
    }
}

void unmount_usb_device(void) {
    if (!g_usb_mounted) {
        return;
    }
    
    vTaskDelay(pdMS_TO_TICKS(500));
    
    g_usb_mounted = false;
    g_current_mount_slot = -1;
    ESP_LOGI(TAG_USB, "🔌 USB демонтовано");
}

static void usb_forget_devices(void)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i]) {
            if (msc_devices[i]->vfs_handle) {
                msc_host_vfs_unregister(msc_devices[i]->vfs_handle);
            }
            if (msc_devices[i]->msc_device) {
                msc_host_uninstall_device(msc_devices[i]->msc_device);
            }
            free(msc_devices[i]);
            msc_devices[i] = NULL;
        }
    }
    g_usb_mounted = false;
    g_current_mount_slot = -1;
}

void usb_task(void *args)
{
    const usb_host_config_t host_config = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_config));

    uint8_t signaled[8] = {0};
    int signaled_count = 0;
    int scan_ticks = 0;
    int scans = 0;
    int reinit_count = 0;
    bool usb_detected = false;
    int last_num_dev = -1;

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);

        // Never tear down the USB host task here: a transient "no clients"
        // event after a stack re-init must not kill USB permanently.

        if (g_usb_reinit_request) {
            g_usb_reinit_request = false;
            if (g_usb_mounted) {
                ESP_LOGW(TAG_USB, "USB re-init skipped: device is mounted");
                continue;
            }
            ESP_LOGW(TAG_USB, "Manual USB stack re-init requested");
            usb_forget_devices();
            msc_host_uninstall();
            usb_host_uninstall();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_err_t re = usb_host_install(&host_config);
            if (re == ESP_OK) re = msc_host_install(&msc_config);
            ESP_LOGW(TAG_USB, "Manual USB re-init result: %s", esp_err_to_name(re));
            signaled_count = 0;
            reinit_count = 0;
            scans = 0;
            scan_ticks = 0;
            continue;
        }

        // While the app is still booting, periodically look for devices that
        // were already connected/enumerated before the MSC driver was ready.
        if (g_mode_external) {
            usb_detected = true;
        }
        if (!usb_detected && !g_usb_mounted && scans < 30 && ++scan_ticks >= 5) {
            scan_ticks = 0;
            scans++;
            uint8_t addr_list[8] = {0};
            int num_dev = 0;
            esp_err_t e = usb_host_device_addr_list_fill(sizeof(addr_list), addr_list, &num_dev);
            if (num_dev != last_num_dev) {
                ESP_LOGI(TAG_USB, "USB scan: %s, devices=%d", esp_err_to_name(e), num_dev);
                last_num_dev = num_dev;
            }
            if (num_dev > 0) {
                usb_detected = true;   // stop scanning once USB is present
            }
            for (int i = 0; i < num_dev && i < (int)sizeof(addr_list); i++) {
                bool known = false;
                for (int k = 0; k < MAX_MSC_DEVICES; k++) {
                    if (msc_devices[k] && msc_devices[k]->usb_addr == addr_list[i]) { known = true; break; }
                }
                for (int k = 0; k < signaled_count; k++) {
                    if (signaled[k] == addr_list[i]) { known = true; break; }
                }
                if (!known) {
                    ESP_LOGI(TAG_USB, "Boot scan: USB device addr=%d, trying MSC", addr_list[i]);
                    app_message_t message = { .id = APP_DEVICE_CONNECTED };
                    message.data.new_dev_address = addr_list[i];
                    if (app_queue) xQueueSend(app_queue, &message, pdMS_TO_TICKS(100));
                    if (signaled_count < (int)sizeof(signaled)) signaled[signaled_count++] = addr_list[i];
                }
            }

            // Nothing detected: the drive stayed powered across MCU reset and
            // did not re-enumerate. Re-initialise the whole USB stack once to
            // force a bus reset / re-enumeration.
            if (!usb_detected && num_dev == 0 && !g_usb_mounted && reinit_count < 3 && scans >= 6) {
                ESP_LOGW(TAG_USB, "No USB device after boot, re-initialising USB stack");
                usb_forget_devices();
                msc_host_uninstall();
                usb_host_uninstall();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_err_t re = usb_host_install(&host_config);
                if (re == ESP_OK) {
                    re = msc_host_install(&msc_config);
                }
                ESP_LOGW(TAG_USB, "USB stack re-init result: %s", esp_err_to_name(re));
                reinit_count++;
                signaled_count = 0;
                scans = 0;
                scan_ticks = 0;
            }
        }
    }

    vTaskDelay(10);
    ESP_LOGI(TAG_USB, "🧹 Деініціалізація USB");
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}


bool mount_usb_device(int slot) {
    if (g_usb_mounted && g_current_mount_slot == slot) {
        return true;
    }
    
    if (g_usb_mounted) {
        unmount_usb_device();
    }
    
    if (slot < 0 || slot >= MAX_MSC_DEVICES || !msc_devices[slot]) {
        ESP_LOGE(TAG_USB, "❌ Невірний слот для монтування: %d", slot);
        return false;
    }
    
    g_current_mount_slot = slot;
    g_usb_mounted = true;
    ESP_LOGI(TAG_USB, "🔌 USB монтовано зі слота %d", slot);
    return true;
}

int8_t find_usb_addr_by_handle(msc_host_device_handle_t handle)
{
    for (int8_t i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i] && msc_devices[i]->msc_device == handle) {
            return msc_devices[i]->usb_addr;
        }
    }
    return -1;
}

int find_free_slot(void)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i] == NULL) {
            return i;
        }
    }
    return -1;
}
