#include "player_wifi.h"
#include "player_nvs.h"
#include "player_ble.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "wifi_can_sniffer.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include <string.h>

static const char *TAG_WIFI = "Player";

esp_err_t try_wifi_connection_with_fallback(void)
{
    int wifi_mode_pref = 0; // 0=AP, 1=STA
    load_wifi_mode_pref(&wifi_mode_pref);
    
    if (wifi_mode_pref == 1) {
        // Спробувати STA в фоновому режимі
        char sta_ssid[32] = "";
        char sta_password[64] = "";
        load_sta_credentials(sta_ssid, sizeof(sta_ssid), sta_password, sizeof(sta_password));
        
        if (strlen(sta_ssid) > 0) {
            ESP_LOGI(TAG_WIFI, "🌐 Запуск фонового підключення до STA: %s", sta_ssid);
            
            // Запускаємо окрему задачу для STA підключення
            if (xTaskCreatePinnedToCore(sta_connection_task, "sta_connect", 4096, NULL, 1, NULL, 0) == pdPASS) {
                ESP_LOGI(TAG_WIFI, "✅ Задача STA підключення запущена");
            } else {
                ESP_LOGE(TAG_WIFI, "❌ Не вдалося створити задачу STA підключення");
                // Fallback to AP
                goto fallback_to_ap;
            }
            return ESP_OK; // Не чекаємо, повертаємося одразу
        } else {
            ESP_LOGW(TAG_WIFI, "⚠️ STA credentials не налаштовані");
        }
    }
    
fallback_to_ap:
    // Fallback to AP
    ESP_LOGI(TAG_WIFI, "📶 Запуск AP режиму");
    char ap_ssid[32] = "";
    char ap_password[64] = "";
    load_ap_credentials(ap_ssid, sizeof(ap_ssid), ap_password, sizeof(ap_password));
    
    esp_err_t ret = wifi_can_switch_to_ap(ap_ssid, ap_password);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG_WIFI, "✅ AP запущено: %s", ap_ssid);
    }
    return ret;
}

esp_err_t get_wifi_settings(char *sta_ssid, size_t sta_ssid_len, char *sta_password, size_t sta_pass_len, 
                           char *ap_ssid, size_t ap_ssid_len, char *ap_password, size_t ap_pass_len, int *mode)
{
    esp_err_t err = load_wifi_mode_pref(mode);
    if (err != ESP_OK) return err;
    
    err = load_sta_credentials(sta_ssid, sta_ssid_len, sta_password, sta_pass_len);
    if (err != ESP_OK) return err;
    
    return load_ap_credentials(ap_ssid, ap_ssid_len, ap_password, ap_pass_len);
}

esp_err_t set_wifi_settings(const char *sta_ssid, const char *sta_password, 
                           const char *ap_ssid, const char *ap_password, int mode)
{
    esp_err_t err = save_wifi_mode_pref(mode);
    if (err != ESP_OK) return err;
    
    err = save_sta_credentials(sta_ssid, sta_password);
    if (err != ESP_OK) return err;
    
    return save_ap_credentials(ap_ssid, ap_password);
}

void sta_connection_task(void *arg)
{
    ESP_LOGI(TAG_WIFI, "🌐 STA підключення: початок фонової задачі");
    
    // Завантажуємо credentials
    char sta_ssid[32] = "";
    char sta_password[64] = "";
    load_sta_credentials(sta_ssid, sizeof(sta_ssid), sta_password, sizeof(sta_password));
    
    if (strlen(sta_ssid) == 0) {
        ESP_LOGW(TAG_WIFI, "🌐 STA підключення: credentials не знайдені");
        vTaskDelete(NULL);
        return;
    }
    
    // Спробувати переключитись в STA
    esp_err_t ret = wifi_can_switch_to_sta(sta_ssid, sta_password);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_WIFI, "🌐 STA підключення: помилка переключення: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    
    // Чекати підключення до 15 секунд (збільшено для DHCP)
    ESP_LOGI(TAG_WIFI, "⏳ STA підключення: очікування 15 сек...");
    vTaskDelay(pdMS_TO_TICKS(15000));
    
    // Перевірити чи отримали IP адресу
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            if (ip_info.ip.addr != 0) {
                ESP_LOGI(TAG_WIFI, "✅ STA підключення: успішно! IP: " IPSTR, IP2STR(&ip_info.ip));
                vTaskDelete(NULL);
                return;
            }
        }
    }
    
    ESP_LOGW(TAG_WIFI, "⚠️ STA підключення: не отримав IP адресу, перехід в AP режим");
    
    // Якщо STA не вдалося, переключаємося в AP режим
    char ap_ssid[32] = "";
    char ap_password[64] = "";
    load_ap_credentials(ap_ssid, sizeof(ap_ssid), ap_password, sizeof(ap_password));
    
    ret = wifi_can_switch_to_ap(ap_ssid, ap_password);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG_WIFI, "✅ STA підключення: fallback до AP режиму: %s", ap_ssid);
    } else {
        ESP_LOGE(TAG_WIFI, "❌ STA підключення: помилка fallback до AP: %s", esp_err_to_name(ret));
    }
    
    vTaskDelete(NULL);
}

