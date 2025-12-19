#ifndef __WIFI_CAN_SNIFFER_H__
#define __WIFI_CAN_SNIFFER_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Сигнатура пакета (совместимо с CAN-Sniffer)
 */
#define CAN_SNIFFER_SIGNATURE           0xAA55AA55u

/**
 * @brief ID пакета управления и статистики
 */
#define CAN_SNIFFER_SERVICE_ID          0x7FFu

/**
 * @brief Размеры буферов для UDP
 */
#define UDP_BUFFER_SIZE                 256
#define UDP_BUFFER_MINIMUM              20

/**
 * @brief Структура CAN-пакета (совместимо с wxCAN-Sniffer)
 * Для ОТПРАВКИ (ESP32→PC): [id(4)] [interval(2)] [length(1)] [data[0..length]]
 * Для ПРИЕМА (PC→ESP32): [id(4)] [reserved(2)] [length(1)] [data[8]]
 */
typedef struct {
    uint32_t id;        /**< CAN идентификатор пакета (4 байта) */
    uint16_t interval;  /**< Интервал между пакетами в мс (2 байта) */
    uint8_t length;     /**< Длина данных (0-8) */
    uint8_t data[8];    /**< Данные пакета (до 8 байт) */
} can_sniffer_frame_t;

/**
 * @brief Структура отправляемого пакета (с сигнатурой)
 */
typedef struct {
    uint32_t signature;         /**< Сигнатура 0xAA55AA55 */
    can_sniffer_frame_t frame;  /**< CAN-пакет */
} can_sniffer_packet_t;

/**
 * @brief Команды управления (через SERVICE_ID)
 */
typedef enum {
    CAN_SNIFFER_CMD_DISCONNECT = 0x00,  /**< Отключиться от CAN-шины */
    CAN_SNIFFER_CMD_CONNECT = 0x01,     /**< Подключиться к CAN-шине */
} can_sniffer_command_t;

/**
 * @brief Конфигурация Wi-Fi CAN-Sniffer
 */
typedef struct {
    const char *wifi_ssid;              /**< SSID точки доступа */
    const char *wifi_password;          /**< Пароль Wi-Fi */
    const char *pc_ip_str;              /**< IP адрес PC для отправки (например, "192.168.1.100") */
    uint16_t udp_port;                  /**< UDP порт (default 0xAA55) */
    void (*on_can_message)(const can_sniffer_frame_t *frame);  /**< Callback для приема CAN-пакетов */
    void (*on_control_command)(uint8_t cmd, const uint8_t *data, uint8_t len);  /**< Callback для команд управления */
} wifi_can_sniffer_config_t;

/**
 * @brief Инициализация Wi-Fi CAN-Sniffer
 * 
 * @param config Конфигурация
 * @return ESP_OK если успешно, иначе код ошибки
 */
esp_err_t wifi_can_sniffer_init(const wifi_can_sniffer_config_t *config);

/**
 * @brief Отправить CAN-пакет через Wi-Fi
 * 
 * @param frame Указатель на CAN-пакет
 * @return ESP_OK если успешно
 */
esp_err_t wifi_can_sniffer_send_frame(const can_sniffer_frame_t *frame);

/**
 * @brief Отправить CAN-пакет с указанными параметрами
 * 
 * @param can_id CAN идентификатор
 * @param dlc Длина данных (0-8)
 * @param data Указатель на данные
 * @return ESP_OK если успешно
 */
esp_err_t wifi_can_sniffer_send(uint32_t can_id, uint8_t dlc, const uint8_t *data);

/**
 * @brief Отправить статистику
 * 
 * @param fps Пакеты в секунду
 * @param bps Байты в секунду
 * @return ESP_OK если успешно
 */
esp_err_t wifi_can_sniffer_send_statistics(uint16_t fps, uint16_t bps);

/**
 * @brief Получить состояние соединения с PC
 * 
 * @return true если PC подключен
 */
bool wifi_can_sniffer_is_connected(void);

/**
 * @brief Принудительно отправить буфер
 */
void wifi_can_sniffer_flush(void);

/**
 * @brief Деинициализация и остановка
 */
void wifi_can_sniffer_deinit(void);

/**
 * @brief Перемкнути Wi-Fi у режим STA та підключитись до мережі (DHCP)
 *
 * @param ssid SSID мережі
 * @param password Пароль мережі (може бути порожнім для відкритої)
 * @return ESP_OK якщо успішно, код помилки інакше
 */
esp_err_t wifi_can_switch_to_sta(const char *ssid, const char *password);

/**
 * @brief Перемкнути Wi-Fi у режим AP з заданим SSID/паролем
 *
 * @param ssid SSID точки доступу
 * @param password Пароль точки доступу
 * @return ESP_OK якщо успішно, код помилки інакше
 */
esp_err_t wifi_can_switch_to_ap(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif

#endif // __WIFI_CAN_SNIFFER_H__
