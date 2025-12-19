#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * UART OTA Protocol для оновлення slave ESP32 через UART
 * 
 * Протокол підтримує:
 * - Передачу великих файлів (до 2 МБ) блоками по 240 байт
 * - Контрольні суми для перевірки цілісності
 * - Повторну передачу при помилках
 * - Розділення OTA команд та звичайних команд керування
 */

// ============ ПРЕФІКСИ КОМАНД ============
#define UART_CMD_PREFIX_NORMAL     0xAA  // Звичайні команди (NEXT/PREV)
#define UART_CMD_PREFIX_OTA        0x55  // OTA команди

// ============ OTA КОМАНДИ ============
#define OTA_CMD_START              0x01  // Початок OTA: [0x55][0x01][size:4 bytes][crc32:4 bytes]
#define OTA_CMD_DATA_BLOCK         0x02  // Блок даних: [0x55][0x02][block_num:2][data_len:1][data:N][crc16:2]
#define OTA_CMD_END                0x03  // Кінець OTA: [0x55][0x03][final_crc32:4]
#define OTA_CMD_VERIFY             0x04  // Запит перевірки: [0x55][0x04]
#define OTA_CMD_ABORT              0x05  // Відміна OTA: [0x55][0x05]

// ============ OTA ВІДПОВІДІ ============
#define OTA_RESP_ACK               0x06  // Підтвердження успішної операції
#define OTA_RESP_NACK              0x07  // Помилка, потрібна повторна передача
#define OTA_RESP_READY             0x08  // Готовність до початку OTA
#define OTA_RESP_COMPLETE          0x09  // OTA успішно завершено
#define OTA_RESP_ERROR             0x0A  // Критична помилка

// ============ ЗВИЧАЙНІ КОМАНДИ ============
#define UART_CMD_NEXT_TRACK        0x04  // Наступний трек
#define UART_CMD_PREV_TRACK        0x05  // Попередній трек
#define UART_CMD_GET_VERSION       0x06  // Отримати версію збірки

// ============ НАЛАШТУВАННЯ ============
#define UART_OTA_BLOCK_SIZE        240   // Розмір блоку даних (без заголовків)
#define UART_OTA_MAX_RETRIES       3     // Максимум спроб повторної передачі
#define UART_OTA_TIMEOUT_MS        1000  // Таймаут очікування відповіді
#define UART_OTA_MAX_SIZE          (2 * 1024 * 1024)  // Максимум 2 МБ

// ============ СТРУКТУРИ ============

// Пакет початку OTA
typedef struct {
    uint8_t prefix;        // 0x55
    uint8_t cmd;           // OTA_CMD_START
    uint32_t total_size;   // Загальний розмір firmware
    uint32_t crc32;        // CRC32 всього firmware
} __attribute__((packed)) ota_start_packet_t;

// Пакет блоку даних
typedef struct {
    uint8_t prefix;        // 0x55
    uint8_t cmd;           // OTA_CMD_DATA_BLOCK
    uint16_t block_num;    // Номер блоку (0-based)
    uint8_t data_len;      // Довжина даних (≤ UART_OTA_BLOCK_SIZE)
    uint8_t data[UART_OTA_BLOCK_SIZE];  // Дані
    uint16_t crc16;        // CRC16 блоку даних
} __attribute__((packed)) ota_data_packet_t;

// Пакет завершення
typedef struct {
    uint8_t prefix;        // 0x55
    uint8_t cmd;           // OTA_CMD_END
    uint32_t final_crc32;  // Фінальна CRC32 для перевірки
} __attribute__((packed)) ota_end_packet_t;

// Відповідь
typedef struct {
    uint8_t prefix;        // 0x55
    uint8_t response;      // OTA_RESP_*
    uint16_t block_num;    // Номер блоку (якщо NACK)
} __attribute__((packed)) ota_response_t;

// Пакет відповіді версії (розширений для детальної інформації)
typedef struct {
    uint8_t prefix;           // 0xAA
    uint8_t cmd;              // UART_CMD_GET_VERSION
    char main_build_date[12]; // Версія основного додатку
    char main_build_time[9];
    char crc_build_date[12];  // Версія CRC компонента
    char crc_build_time[9];
    char uart_build_date[12]; // Версія UART OTA компонента
    char uart_build_time[9];
} __attribute__((packed)) version_response_t;

// ============ ФУНКЦІЇ CRC ============

/**
 * Обчислити CRC16 для даних
 */
uint16_t uart_ota_crc16(const uint8_t *data, size_t len);

/**
 * Обчислити CRC32 для даних
 */
uint32_t uart_ota_crc32(const uint8_t *data, size_t len);
