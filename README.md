# ESP32 Bluetooth Audio System з USB флешкою

Українська версія 

## 📋 Огляд проекту

Цей проект складається з двох ESP32 мікроконтролерів, які працюють разом для створення Bluetooth аудіо системи з відтворенням музики з флешки usb. Проект базується на **ESP-ADF (Espressif Audio Development Framework)** для обробки аудіо та Bluetooth стеків.

- **Master ESP32** (pipeline_play_sdcard_music): Основний пристрій з USB флешкою, веб-інтерфейсом та керуванням
- **Slave ESP32** (esp32-bluetooth): Bluetooth-приймач A2DP/HFP, передає аудіо через UART

## 🔌 Підключення апаратного забезпечення

### Master ESP32 (ESP32-S3 з USB флешкою)
```
ESP32-S3                    Підключення
=========                   ===========
GPIO 8   ────────────────── UART TX ── Slave ESP32 GPIO 22 (керування)
GPIO 9   ────────────────── UART RX ── Slave ESP32 GPIO 23 (керування)
GPIO 41  ────────────────── I2S BCLK ─ Slave ESP32 GPIO 26 (аудіо)
GPIO 39  ────────────────── I2S WS ─── Slave ESP32 GPIO 33 (аудіо)
GPIO 40  ────────────────── I2S DIN ── Slave ESP32 GPIO 25 (аудіо)
GND      ────────────────── GND ────── Slave ESP32 GND
3.3V     ────────────────── 3.3V ───── Slave ESP32 3.3V

USB Flash Drive (USB OTG):
GPIO 19  ────────────────── USB D- ── USB Flash Drive D-
GPIO 20  ────────────────── USB D+ ── USB Flash Drive D+
GND      ────────────────── GND ───── USB Flash Drive GND
5V       ────────────────── 5V ────── USB Flash Drive VCC
```

### Slave ESP32 (Bluetooth-приймач)
```
ESP32                       Підключення
=====                       ===========
GPIO 23  ────────────────── UART TX ── Master ESP32 GPIO 9 (керування)
GPIO 22  ────────────────── UART RX ── Master ESP32 GPIO 8 (керування)
GPIO 26  ────────────────── I2S BCLK ─ Master ESP32 GPIO 41 (аудіо)
GPIO 33  ────────────────── I2S WS ─── Master ESP32 GPIO 39 (аудіо)
GPIO 25  ────────────────── I2S DOUT ─ Master ESP32 GPIO 40 (аудіо)
GND      ────────────────── GND ────── Master ESP32 GND
3.3V     ────────────────── 3.3V ───── Master ESP32 3.3V

Bluetooth: Вбудований модуль ESP32
Speaker: Підключений до аудіо кодека
```

### UART конфігурація (керування)
- **Швидкість**: 115200 bps
- **Дані**: 8 біт
- **Парність**: Відсутня
- **Стоп-біт**: 1
- **Потік**: Без контролю
- **Призначення**: Команди керування, OTA оновлення, статус

### I2S конфігурація (аудіо)
- **Швидкість**: 44.1 kHz (або 48 kHz)
- **Розрядність**: 16 біт
- **Канали**: Stereo
- **Призначення**: Передача аудіо від Slave до Master

## 🎵 Підтримувані формати музики

| № | Формат | Кодек |
|---|--------|-------|
| 1 | MP3    | MP3   |
| 2 | AMR-NB | AMR   |
| 3 | AMR-WB | AMR   |
| 4 | OPUS   | OPUS  |
| 5 | FLAC   | FLAC  |
| 6 | WAV    | PCM   |
| 7 | AAC    | AAC   |
| 8 | M4A    | AAC   |
| 9 | TS     | MPEG-TS |
| 10| MP4    | MPEG-4 |

## 🚀 Швидкий старт

### 1. Підготовка USB флешки
1. Відформатуйте USB флешку у FAT32
2. Завантажте аудіофайли у підтримувані формати (MP3, FLAC, WAV тощо)
3. Помістіть файли в корінь флешки

### 2. Збірка та прошивка

#### Master ESP32:
```bash
cd d:\emulators\pipeline_play_sdcard_music
idf.py build
idf.py -p COM_MASTER flash
```

#### Slave ESP32:
```bash
cd d:\emulators\esp32-bluetooth
idf.py build
idf.py -p COM_SLAVE flash
```

### 3. Налаштування Wi-Fi
Після запуску Master ESP32 створить Wi-Fi точку доступу:
- **SSID**: ESP32-MusicPlayer
- **Пароль**: music1234

Підключіться та відкрийте `http://192.168.4.1/`

## 🌐 Веб-інтерфейс

### Основні функції:
- **Відтворення музики**: Кнопки play/pause, попередній/наступний трек
- **Плейліст**: Автоматичне сканування USB флешки
- **Bluetooth керування**: Підключення до Bluetooth пристроїв
- **OTA оновлення**: Безпровідне оновлення прошивки
- **Системна інформація**: Версії компонентів, статус

### OTA оновлення:
1. Виберіть цільовий пристрій (Master або Slave)
2. Оберіть файл прошивки (.bin)
3. Натисніть "Upload & Update"
4. Процес показується з індикатором прогресу

## 📋 Архітектура системи

```
[Bluetooth Device] ── A2DP ──► [Slave ESP32] ── I2S ──► [Master ESP32] ── I2S ──► [Audio Codec] ──► [Speakers]
                                    │                        │
                                    ▼                        ▼
                            [UART керування]        [USB Flash Music]
                                                     [Web Interface]
                                                     [Wi-Fi AP/STA]
```

### Комунікаційні протоколи:
- **Bluetooth**: A2DP (Advanced Audio Distribution Profile) + AVRCP (Audio/Video Remote Control Profile) + HFP (Hands-Free Profile)
- **I2S**: Передача аудіо сигналу між ESP32 (44.1kHz, 16bit, Stereo)
- **UART**: Керування та команди між пристроями (115200 bps)
- **HTTP**: Веб-інтерфейс через ESP HTTP Server
- **WebSocket**: Реальний час оновлення статусу

## ⚙️ Конфігурація

### Master ESP32 (menuconfig):
```bash
menuconfig > Audio HAL > ESP32-Lyrat V4.3 (або інша плата)
menuconfig > Component config > FAT Filesystem support > Long filename support
```

### Slave ESP32 (menuconfig):
```bash
menuconfig > Audio HAL > ESP32-Lyrat-Mini V1.1 (або інша плата)
```

## 🔧 Розширена конфігурація

### Зміна Wi-Fi налаштувань:
Через веб-інтерфейс або у коді `main/play_sdcard_music_example.c`:
```c
#define WIFI_SSID "Your_SSID"
#define WIFI_PASSWORD "Your_Password"
```

### Налаштування UART:
```c
// Master
#define MASTER_UART_TX_PIN 8
#define MASTER_UART_RX_PIN 9
#define MASTER_UART_BAUD 115200

// Slave
#define APP_UART_TX_PIN GPIO_NUM_23
#define APP_UART_RX_PIN GPIO_NUM_22
#define APP_UART_BAUD 115200
```

## 🐛 Вирішення проблем

### Master ESP32 не запускається:
- Перевірте USB флешку (FAT32, наявність аудіофайлів)
- Перевірте UART підключення (GPIO 8,9)
- Перевірте I2S підключення (GPIO 39,40,41)
- Перевірте живлення (3.3V стабільне)

### Slave ESP32 не підключається по Bluetooth:
- Перевірте UART з'єднання (GPIO 8,9)
- Перевірте I2S з'єднання (GPIO 39,40,41)
- Перевірте живлення slave ESP32
- Перевірте Bluetooth сумісність

### Веб-інтерфейс не працює:
- Перевірте підключення до Wi-Fi AP
- Перевірте IP адресу (192.168.4.1)
- Очистіть кеш браузера

### OTA не працює:
- Перевірте розмір файлу (не більше partition size)
- Перевірте MD5 суму файлу
- Перевірте мережеве з'єднання

## 📊 Системні вимоги

- **ESP32-S3** або **ESP32** для Master
- **ESP32** для Slave
- **USB флешка** (FAT32, до 32GB)
- **Bluetooth джерело** (телефон, комп'ютер)
- **Аудіо підсилювач** та **динаміки**
- **3.3V джерело живлення** (мінімум 1A)
- **Підключення**: UART (GPIO 8,9) + I2S (GPIO 39,40,41)

## 🔄 Версії компонентів

Проект використовує:
- **ESP-IDF**: v5.3+
- **ESP-ADF**: Audio Development Framework
- **FreeRTOS**: Множинні задачі для асинхронної роботи
- **FATFS**: Файлова система для USB флешки
- **Bluetooth Stack**: Classic Bluetooth з A2DP/AVRCP/HFP

## 📝 Ліцензія

Unlicense OR CC0-1.0

## 🤝 Внесок

Для внеску у проект:
1. Fork репозиторій
2. Створіть feature branch
3. Commit зміни
4. Push та створіть Pull Request

## 📞 Підтримка

При виникненні проблем:
1. Перевірте логи ESP32 через UART
2. Перевірте підключення апаратного забезпечення
3. Створіть issue з детальним описом
