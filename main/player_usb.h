#pragma once
#include "esp_err.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "sdkconfig.h"
#include <stdint.h>
#include "app_messages.h"

#define MNT_PATH "/ROOT"
#define MAX_MSC_DEVICES CONFIG_FATFS_VOLUME_COUNT

int find_free_slot(void);
extern bool g_usb_mounted;
extern volatile bool g_usb_reinit_request;
extern int g_current_mount_slot;

typedef struct {
    uint8_t usb_addr;
    msc_host_device_handle_t msc_device;
    msc_host_vfs_handle_t vfs_handle;
} msc_dev_entry_t;

extern msc_dev_entry_t *msc_devices[MAX_MSC_DEVICES];

esp_err_t allocate_new_msc_device(const app_message_t *msg, int *out_slot, bool *is_new);
int find_slot_by_handle(msc_host_device_handle_t handle);
void free_msc_device(int slot);
void free_all_msc_devices(void);
int8_t find_usb_addr_by_handle(msc_host_device_handle_t handle);
bool mount_usb_device(int slot);
void msc_event_cb(const msc_host_event_t *event, void *arg);

void unmount_usb_device(void);
void usb_task(void *pvParameters);
