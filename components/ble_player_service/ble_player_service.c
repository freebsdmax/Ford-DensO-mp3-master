#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"

#include "ble_player_service.h"

static const char *TAG = "BLE_Player";

// Profile attributes
#define PLAYER_PROFILE_NUM 1
#define PLAYER_PROFILE_APP_ID 0
#define PLAYER_SVC_INST_ID 0
#define BLE_RPC_QUEUE_LENGTH 8
#define BLE_RPC_REQUEST_SIZE 1200

typedef struct {
    esp_gatt_if_t gatts_if;
    uint16_t conn_id;
    char request[BLE_RPC_REQUEST_SIZE];
} ble_rpc_request_t;

// 128-bit UUID of the player service: 11000000-0000-1000-8000-00805f9b34fb
static const uint8_t PLAYER_SERVICE_UUID128[16] = {
    0xfb, 0x34, 0x9b, 0x5f,
    0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x11
};

// 128-bit UUID for characteristics
static const uint8_t COMMAND_CHAR_UUID128[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x01, 0x01, 0x11
};

static const uint8_t STATUS_CHAR_UUID128[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x02, 0x01, 0x11
};

static const uint8_t PLAYLIST_CHAR_UUID128[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x03, 0x01, 0x11
};

static const uint8_t LOGS_CHAR_UUID128[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x04, 0x01, 0x11
};

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle_cmd;
    uint16_t char_handle_status;
    uint16_t char_handle_playlist;
    uint16_t char_handle_logs;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};

static ble_command_callback_t g_command_callback = NULL;
static ble_status_callback_t g_status_callback = NULL;
static ble_playlist_callback_t g_playlist_callback = NULL;
static ble_rpc_callback_t g_rpc_callback = NULL;
static QueueHandle_t g_rpc_queue = NULL;

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

#define OTA_FRAME_MAGIC 0xA5
#define OTA_FRAME_DATA  0x01
#define OTA_FRAME_HDR   8

static ble_ota_frame_callback_t s_ota_frame_cb = NULL;
static int s_ota_frame_errors = 0;

static struct gatts_profile_inst player_profile_tab[PLAYER_PROFILE_NUM] = {
    [PLAYER_PROFILE_APP_ID] = {
        .gatts_cb = gatts_profile_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,
    },
};

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch(event) {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            ESP_LOGI(TAG, "Advertising data set");
            break;
        case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
            ESP_LOGI(TAG, "Scan response data set");
            esp_ble_gap_start_advertising(&(esp_ble_adv_params_t){
                .adv_int_min = 0x20,
                .adv_int_max = 0x40,
                .adv_type = ADV_TYPE_IND,
                .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                .channel_map = ADV_CHNL_ALL,
                .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
            });
            break;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "✅ BLE advertising started");
            } else {
                ESP_LOGE(TAG, "❌ BLE advertising start failed");
            }
            break;
        default:
            break;
    }
}

static void send_status_notification(esp_gatt_if_t gatts_if)
{
    struct gatts_profile_inst *profile = &player_profile_tab[PLAYER_PROFILE_APP_ID];
    if (!g_status_callback || profile->char_handle_status == 0) {
        return;
    }

    ble_player_status_t status = {0};
    char payload[64];
    g_status_callback(&status);
    int length = snprintf(payload, sizeof(payload), "%d,%d,%d,%s,%d,%s,%d",
                          status.current_track, status.position_ms, status.duration_ms,
                          status.is_paused ? "true" : "false", status.playlist_size,
                          status.mode_external ? "external" : "USB", status.sniffer_enabled);
    if (length > 0 && length < sizeof(payload)) {
        esp_ble_gatts_send_indicate(gatts_if, profile->conn_id, profile->char_handle_status,
                                    length, (uint8_t *)payload, false);
    }
}

static void send_playlist_notification(esp_gatt_if_t gatts_if)
{
    struct gatts_profile_inst *profile = &player_profile_tab[PLAYER_PROFILE_APP_ID];
    if (!g_playlist_callback || profile->char_handle_playlist == 0) {
        return;
    }

    char payload[GATTS_DEMO_CHAR_VAL_LEN_MAX] = {0};
    size_t length = g_playlist_callback(payload, sizeof(payload));
    if (length >= sizeof(payload)) {
        length = sizeof(payload) - 1;
    }
    if (length > 0) {
        esp_ble_gatts_send_indicate(gatts_if, profile->conn_id, profile->char_handle_playlist,
                                    length, (uint8_t *)payload, false);
    }
}

static void ble_rpc_worker(void *arg)
{
    (void)arg;
    ble_rpc_request_t request;
    struct gatts_profile_inst *profile = &player_profile_tab[PLAYER_PROFILE_APP_ID];

    while (true) {
        if (xQueueReceive(g_rpc_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!g_rpc_callback || profile->char_handle_logs == 0) {
            continue;
        }

        char response[GATTS_DEMO_CHAR_VAL_LEN_MAX] = {0};
        size_t length = g_rpc_callback(request.request, response, sizeof(response));
        if (length >= sizeof(response)) {
            length = sizeof(response) - 1;
        }
        if (length > 0) {
            esp_err_t err = esp_ble_gatts_send_indicate(request.gatts_if, request.conn_id,
                                                        profile->char_handle_logs, length,
                                                        (uint8_t *)response, false);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "BLE RPC response send failed: %s", esp_err_to_name(err));
            }
        }
    }
}

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
        case ESP_GATTS_REG_EVT:
            ESP_LOGI(TAG, "GATT service registered, gatts_if=%d", gatts_if);
            player_profile_tab[PLAYER_PROFILE_APP_ID].service_id.is_primary = true;
            player_profile_tab[PLAYER_PROFILE_APP_ID].service_id.id.inst_id = PLAYER_SVC_INST_ID;
            player_profile_tab[PLAYER_PROFILE_APP_ID].service_id.id.uuid.len = ESP_UUID_LEN_128;
            memcpy(player_profile_tab[PLAYER_PROFILE_APP_ID].service_id.id.uuid.uuid.uuid128, PLAYER_SERVICE_UUID128, ESP_UUID_LEN_128);

            esp_ble_gap_set_device_name("ESP32_MP3_Player");
            
            esp_ble_gatts_create_service(gatts_if, &player_profile_tab[PLAYER_PROFILE_APP_ID].service_id, 12);
            break;

        case ESP_GATTS_CREATE_EVT:
            ESP_LOGI(TAG, "✅ Service created, handle=%d", param->create.service_handle);
            player_profile_tab[PLAYER_PROFILE_APP_ID].service_handle = param->create.service_handle;
            esp_ble_gatts_start_service(player_profile_tab[PLAYER_PROFILE_APP_ID].service_handle);

            // Add COMMAND characteristic
            esp_bt_uuid_t cmd_uuid = {.len = ESP_UUID_LEN_128};
            memcpy(cmd_uuid.uuid.uuid128, COMMAND_CHAR_UUID128, ESP_UUID_LEN_128);
            esp_ble_gatts_add_char(player_profile_tab[PLAYER_PROFILE_APP_ID].service_handle,
                                   &cmd_uuid,
                                   ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                   ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
                                   NULL, NULL);
            break;

        case ESP_GATTS_ADD_CHAR_EVT:
            ESP_LOGI(TAG, "Characteristic added, handle=%d", param->add_char.attr_handle);
            if (player_profile_tab[PLAYER_PROFILE_APP_ID].char_handle_cmd == 0) {
                player_profile_tab[PLAYER_PROFILE_APP_ID].char_handle_cmd = param->add_char.attr_handle;
                
                // Add STATUS characteristic
                esp_bt_uuid_t status_uuid = {.len = ESP_UUID_LEN_128};
                memcpy(status_uuid.uuid.uuid128, STATUS_CHAR_UUID128, ESP_UUID_LEN_128);
                esp_ble_gatts_add_char(player_profile_tab[PLAYER_PROFILE_APP_ID].service_handle,
                                       &status_uuid,
                                       ESP_GATT_PERM_READ,
                                       ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                                       NULL, NULL);
                // Add CCCD for STATUS
                esp_bt_uuid_t cccd_uuid = {.len = ESP_UUID_LEN_16, .uuid = {.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG}};
                esp_ble_gatts_add_char_descr(player_profile_tab[PLAYER_PROFILE_APP_ID].service_handle,
                                             &cccd_uuid,
                                             ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                             NULL, NULL);
            } else if (player_profile_tab[PLAYER_PROFILE_APP_ID].char_handle_status == 0) {
                player_profile_tab[PLAYER_PROFILE_APP_ID].char_handle_status = param->add_char.attr_handle;
                
                // Add PLAYLIST characteristic
                esp_bt_uuid_t playlist_uuid = {.len = ESP_UUID_LEN_128};
                memcpy(playlist_uuid.uuid.uuid128, PLAYLIST_CHAR_UUID128, ESP_UUID_LEN_128);
                esp_ble_gatts_add_char(player_profile_tab[PLAYER_PROFILE_APP_ID].service_handle,
                                       &playlist_uuid,
                                       ESP_GATT_PERM_READ,
                                       ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                                       NULL, NULL);
                // Add CCCD for PLAYLIST
                esp_bt_uuid_t cccd_uuid = {.len = ESP_UUID_LEN_16, .uuid = {.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG}};
                esp_ble_gatts_add_char_descr(player_profile_tab[PLAYER_PROFILE_APP_ID].service_handle,
                                             &cccd_uuid,
                                             ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                             NULL, NULL);
            } else if (player_profile_tab[PLAYER_PROFILE_APP_ID].char_handle_playlist == 0) {
                player_profile_tab[PLAYER_PROFILE_APP_ID].char_handle_playlist = param->add_char.attr_handle;
                
                // Add LOGS characteristic
                esp_bt_uuid_t logs_uuid = {.len = ESP_UUID_LEN_128};
                memcpy(logs_uuid.uuid.uuid128, LOGS_CHAR_UUID128, ESP_UUID_LEN_128);
                esp_ble_gatts_add_char(player_profile_tab[PLAYER_PROFILE_APP_ID].service_handle,
                                       &logs_uuid,
                                       ESP_GATT_PERM_READ,
                                       ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                                       NULL, NULL);
                // Add CCCD for LOGS
                esp_bt_uuid_t cccd_uuid = {.len = ESP_UUID_LEN_16, .uuid = {.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG}};
                esp_ble_gatts_add_char_descr(player_profile_tab[PLAYER_PROFILE_APP_ID].service_handle,
                                             &cccd_uuid,
                                             ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                             NULL, NULL);
            } else {
                player_profile_tab[PLAYER_PROFILE_APP_ID].char_handle_logs = param->add_char.attr_handle;
                ESP_LOGI(TAG, "✅ All characteristics added");
            }
            break;

        case ESP_GATTS_ADD_CHAR_DESCR_EVT:
            ESP_LOGI(TAG, "Descriptor added, handle=%d", param->add_char_descr.attr_handle);
            break;

        case ESP_GATTS_CONNECT_EVT:
            ESP_LOGI(TAG, "✅ BLE client connected, conn_id=%d", param->connect.conn_id);
            player_profile_tab[PLAYER_PROFILE_APP_ID].conn_id = param->connect.conn_id;
            // Prefer LE 2M PHY for faster transfers (BLE 5)
            esp_ble_gap_set_preferred_phy(param->connect.remote_bda, 0,
                                          ESP_BLE_GAP_PHY_2M, ESP_BLE_GAP_PHY_2M,
                                          ESP_BLE_GAP_PHY_OPTIONS_NO_PREF);
            
            // Продовжити рекламу для видимості іншим пристроям
            esp_ble_gap_start_advertising(&(esp_ble_adv_params_t){
                .adv_int_min = 0x20,
                .adv_int_max = 0x40,
                .adv_type = ADV_TYPE_IND,
                .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                .channel_map = ADV_CHNL_ALL,
                .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
            });
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            ESP_LOGI(TAG, "🔌 BLE client disconnected");
            esp_ble_gap_start_advertising(&(esp_ble_adv_params_t){
                .adv_int_min = 0x20,
                .adv_int_max = 0x40,
                .adv_type = ADV_TYPE_IND,
                .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                .channel_map = ADV_CHNL_ALL,
                .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
            });
            break;

        case ESP_GATTS_WRITE_EVT:
            if (param->write.handle == player_profile_tab[PLAYER_PROFILE_APP_ID].char_handle_cmd &&
                param->write.len >= OTA_FRAME_HDR &&
                param->write.value[0] == OTA_FRAME_MAGIC) {
                const uint8_t *v = param->write.value;
                uint32_t off = (uint32_t)v[2] | ((uint32_t)v[3] << 8) |
                               ((uint32_t)v[4] << 16) | ((uint32_t)v[5] << 24);
                size_t len = (size_t)v[6] | ((size_t)v[7] << 8);
                bool ok = false;
                if (v[1] != OTA_FRAME_DATA) {
                    if (s_ota_frame_errors++ < 10) {
                        ESP_LOGW(TAG, "OTA frame: unknown type 0x%02X", v[1]);
                    }
                } else if (len == 0 || len + OTA_FRAME_HDR > param->write.len) {
                    if (s_ota_frame_errors++ < 10) {
                        ESP_LOGW(TAG, "OTA frame: bad length %u (write %u)",
                                 (unsigned)len, (unsigned)param->write.len);
                    }
                } else if (s_ota_frame_cb == NULL) {
                    if (s_ota_frame_errors++ < 10) {
                        ESP_LOGW(TAG, "OTA frame: no handler registered");
                    }
                } else {
                    ok = s_ota_frame_cb(off, v + OTA_FRAME_HDR, len);
                    if (!ok && s_ota_frame_errors++ < 10) {
                        ESP_LOGW(TAG, "OTA frame rejected: off=%u len=%u",
                                 (unsigned)off, (unsigned)len);
                    }
                }
                if (param->write.need_rsp) {
                    esp_gatt_rsp_t rsp = {0};
                    rsp.attr_value.handle = param->write.handle;
                    rsp.attr_value.len = 0;
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                                param->write.trans_id,
                                                ok ? ESP_GATT_OK : ESP_GATT_INVALID_ATTR_LEN, &rsp);
                }
                break;
            }
            if (param->write.handle == player_profile_tab[PLAYER_PROFILE_APP_ID].char_handle_cmd) {
                //ESP_LOGI(TAG, "BLE command write received: %u bytes", (unsigned)param->write.len);
                esp_gatt_status_t write_status = ESP_GATT_OK;

                // Long writes arrive as prepared fragments; without reassembly they look
                // like broken commands. Log them so the app side can be diagnosed.
                if (param->write.is_prep) {
                    ESP_LOGW(TAG, "prepared BLE write: %u bytes (long write fragment)",
                             (unsigned)param->write.len);
                } else if (param->write.len > 0 &&
                           strncmp((const char *)param->write.value, "rpc:file.data", 13) == 0 &&
                           (param->write.len < 120 || param->write.len > 480)) {
                    ESP_LOGW(TAG, "file.data write: %u bytes (mtu-limited write is ~509 max)",
                             (unsigned)param->write.len);
                }

                // Static: a ~1.2 KB buffer on the Bluedroid callback stack is too much.
                static char s_cmd[BLE_RPC_REQUEST_SIZE + 1];
                memset(s_cmd, 0, sizeof(s_cmd));
                char *cmd = s_cmd;
                size_t copy_len = param->write.len < sizeof(s_cmd) - 1 ? param->write.len : sizeof(s_cmd) - 1;
                memcpy(cmd, param->write.value, copy_len);
                while (copy_len > 0 && (cmd[copy_len - 1] == '\r' || cmd[copy_len - 1] == '\n')) {
                    cmd[--copy_len] = '\0';
                }
                char *params = strchr(cmd, ':');
                if (params) {
                    *params++ = '\0';
                }

                // Обробити команди status і playlist безпосередньо
                if (strcmp(cmd, "status") == 0) {
                    send_status_notification(gatts_if);
                } else if (strcmp(cmd, "playlist") == 0) {
                    send_playlist_notification(gatts_if);
                } else if (strcmp(cmd, "rpc") == 0 && params && g_rpc_callback) {
                    size_t request_length = strlen(params);
                    if (request_length >= BLE_RPC_REQUEST_SIZE || g_rpc_queue == NULL) {
                        write_status = ESP_GATT_INVALID_ATTR_LEN;
                    } else {
                        ble_rpc_request_t request = {
                            .gatts_if = gatts_if,
                            .conn_id = param->write.conn_id,
                        };
                        memcpy(request.request, params, request_length + 1);
                        if (xQueueSend(g_rpc_queue, &request, 0) != pdTRUE) {
                            ESP_LOGW(TAG, "BLE RPC queue full; request rejected");
                            write_status = ESP_GATT_BUSY;
                        }
                    }
                } else if (g_command_callback && cmd[0] != '\0') {
                    // Інші команди передати callback (play, pause, next, etc)
                    ESP_LOGI(TAG, "▶️ Processing command: %s (params: %s)", cmd, params ? params : "");
                    g_command_callback(cmd, params);
                }

                // Ответить на запись, если клиент ожидает rsp
                if (param->write.need_rsp) {
                    esp_gatt_rsp_t rsp = {0};
                    rsp.attr_value.handle = param->write.handle;
                    rsp.attr_value.len = 0;
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, write_status, &rsp);
                }
            }
            break;

        case ESP_GATTS_READ_EVT:
            ESP_LOGI(TAG, "📖 Read request on handle=%d", param->read.handle);
            if (param->read.handle == player_profile_tab[PLAYER_PROFILE_APP_ID].char_handle_status) {
                send_status_notification(gatts_if);
            } else if (param->read.handle == player_profile_tab[PLAYER_PROFILE_APP_ID].char_handle_playlist) {
                send_playlist_notification(gatts_if);
            }
            break;

        default:
            break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            player_profile_tab[param->reg.app_id].gatts_if = gatts_if;
        } else {
            ESP_LOGE(TAG, "Reg app failed, app_id=%04x, status=%d", param->reg.app_id, param->reg.status);
            return;
        }
    }

    for (int idx = 0; idx < PLAYER_PROFILE_NUM; idx++) {
        if (gatts_if == ESP_GATT_IF_NONE || gatts_if == player_profile_tab[idx].gatts_if) {
            if (player_profile_tab[idx].gatts_cb) {
                player_profile_tab[idx].gatts_cb(event, gatts_if, param);
            }
        }
    }
}

esp_err_t ble_player_service_init(void)
{
    esp_err_t ret = ESP_OK;
    
    // Release memory for classic BT
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    
    // Initialize BT controller with default config
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Enable BT controller in BLE mode
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize Bluedroid
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gatt_set_local_mtu(512);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set local GATT MTU failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (g_rpc_queue == NULL) {
        g_rpc_queue = xQueueCreate(BLE_RPC_QUEUE_LENGTH, sizeof(ble_rpc_request_t));
        if (g_rpc_queue == NULL) {
            ESP_LOGE(TAG, "BLE RPC queue allocation failed");
            return ESP_ERR_NO_MEM;
        }
        if (xTaskCreate(ble_rpc_worker, "ble_rpc", 8192, NULL, 5, NULL) != pdPASS) {
            vQueueDelete(g_rpc_queue);
            g_rpc_queue = NULL;
            ESP_LOGE(TAG, "BLE RPC worker task creation failed");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Register callbacks
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "GAP register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "GATTS register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Register GATT application profile
    ret = esp_ble_gatts_app_register(PLAYER_PROFILE_APP_ID);
    if (ret) {
        ESP_LOGE(TAG, "GATTS app register failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Configure advertising data (UUID + flags, keep payload under 31 bytes)
    esp_ble_adv_data_t adv_data = {
        .set_scan_rsp = false,
        .include_name = false,
        .include_txpower = false,
        .min_interval = 0x0006,
        .max_interval = 0x0010,
        .appearance = 0x00,
        .manufacturer_len = 0,
        .p_manufacturer_data = NULL,
        .service_data_len = 0,
        .p_service_data = NULL,
        .service_uuid_len = sizeof(PLAYER_SERVICE_UUID128),
        .p_service_uuid = (uint8_t*)PLAYER_SERVICE_UUID128,
        .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    };

    esp_ble_adv_data_t scan_rsp_data = {
        .set_scan_rsp = true,
        .include_name = true,
        .include_txpower = true,
    };

    ret = esp_ble_gap_config_adv_data(&adv_data);
    if (ret) {
        ESP_LOGE(TAG, "Config adv data failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
    if (ret) {
        ESP_LOGE(TAG, "Config scan rsp data failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "BLE Player Service initialized successfully");
    return ESP_OK;
}

void ble_player_set_command_handler(ble_command_callback_t callback)
{
    g_command_callback = callback;
    ESP_LOGI(TAG, "BLE command handler registered");
}

void ble_player_start_periodic_status(ble_status_callback_t callback, int interval_ms)
{
    g_status_callback = callback;
    ESP_LOGI(TAG, "BLE periodic status started: %d ms", interval_ms);
}

void ble_player_set_playlist_callback(ble_playlist_callback_t callback)
{
    g_playlist_callback = callback;
}

void ble_player_set_rpc_callback(ble_rpc_callback_t callback)
{
    g_rpc_callback = callback;
}

void ble_player_set_ota_frame_handler(ble_ota_frame_callback_t callback)
{
    s_ota_frame_cb = callback;
}
