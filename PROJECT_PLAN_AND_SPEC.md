# ПОЛНОЕ ТЕХНИЧЕСКОЕ ЗАДАНИЕ И АРХИТЕКТУРНЫЙ ПЛАН
# Медиасистема Ford Denso: Master (ESP32-S3) + Slave (ESP32 BT) + Android App

> **Назначение документа:** Полное техническое описание аппаратной части, архитектуры, протоколов взаимодействия и пошаговых алгоритмов работы **всей экосистемы**: Master ESP32-S3, Slave ESP32 и Android-приложения (`AndroidPlayer`). Документ самодостаточен и может быть передан любому AI-агенту или разработчику для создания новой модульной кодовой базы.

---

## 1. ОБЩИЙ ОБЗОР И РОЛИ КОМПОНЕНТОВ

Система эмулирует штатный медиаплеер для головных устройств автомобилей Ford (с шиной CAN магнитол Ford Denso / Visteon) и распределена между тремя ключевыми узлами:

```
[ Смартфон: Музыка (A2DP) ] 
        │ 
        ▼ 
[ ESP32 SLAVE BT ] (Приемник A2DP "ForD DensO")
        │ 
        ├─── I2S Звук (GPIO 26, 33, 25) ─────► I2S Вход (GPIO 41, 39, 40) ────┐
        ├─── UART Команды/OTA (GPIO 22, 23) ◄──► UART Связь (GPIO 8, 9) ──────┤
        └─── Sleep Sync Вход (GPIO 34) ◄────── Sleep Sync Выход (GPIO 21) ──┤
                                                                            ▼
                                                                 [ ESP32-S3 MASTER ]
                                                                    │   │   │
                      CAN-шина Ford (GPIO 4 TX, 15 RX) ─────────────┘   │   └── USB Flash OTG (GPIO 19, 20)
                      I2S Выход на ЦАП PCM5102 (GPIO 16, 17, 18) ───────┘
                                                                            │
                                                        ┌───────────────────┴───────────────────┐
                                                        ▼ (BLE GATT)                            ▼ (Wi-Fi HTTP REST)
                                            [ ANDROID-ПРИЛОЖЕНИЕ: AndroidPlayer (Jetpack Compose) ]
```

### Роль 1: Master ESP32-S3
- **Хранилище и плеер:** Читает USB-флешку (FAT32) через USB-Host OTG. Воспроизводит аудио (MP3) через пайплайн ESP-ADF.
- **ЦАП (Выход звука):** Тактирует внешний аудио-ЦАП PCM5102 по I2S0 Master (44.1/48 kHz, 16 bit Stereo).
- **Вход внешнего звука:** Принимает I2S1 Slave поток от ESP32 Slave и транслирует в ЦАП в режиме passthrough.
- **CAN-шина Ford:** Подключается к CAN-шине (125 kbps, ID `0x2D5`), обрабатывает кнопки руля/магнитолы, фильтрует дребезг (debounce).
- **Синхронизация питания:** При отсутствии CAN уходит в Deep Sleep и сигналом через `GPIO 21` отправляет Slave чип в Deep Sleep. Просыпается от CAN RX (`GPIO 15`).
- **Связь и OTA со Slave:** Передает команды Next/Prev по UART1 и выполняет поблочную OTA-прошивку Slave платы.
- **Сетевые сервисы:** Wi-Fi SoftAP/STA, Web UI панель, Web OTA, UDP CAN Sniffer (wxCAN-Sniffer), TCP ELM327 сервер (диагностика), BLE GATT сервер для мобильного приложения.

### Роль 2: Slave ESP32 (Bluetooth Audio Sink)
- **Bluetooth Стек:** Приемник A2DP Sink (имя `"ForD DensO"`) и профиль управления AVRCP.
- **Авто-переподключение (Auto-reconnect):** Сохраняет MAC-адрес последнего подключенного смартфона в NVS (`a2dp_sink`). При старте фоновая задача `bt_reconnect_task` автоматически восстанавливает соединение.
- **Аудио-выход:** Передает декодированный PCM-звук по аппаратному I2S0 (Master TX) на Master ESP32-S3.
- **UART Командный обработчик (`uart_cmd_task`):**
  - Принимает обычные команды управления (`0xAA 0x04` Next, `0xAA 0x05` Prev) и переключает треки на смартфоне через AVRCP.
  - Принимает запрос версии (`0xAA 0x06`) и возвращает структуру версий сборки.
  - Принимает поток OTA-обновления (`0x55 ...`) и прошивает свой Flash-раздел (`ota_0`/`ota_1`) с верификацией CRC16/CRC32.
- **Контроль питания:** Задача `i2c_sleep_monitor_task` опрашивает `GPIO 34`. Как только Master переводит линию в LOW, Slave уходит в Deep Sleep. Просыпается по уровню на `GPIO 34`.

### Роль 3: Android-приложение (`AndroidPlayer`)
- **Стек:** Kotlin, Jetpack Compose, Material 3, Coroutines, StateFlow, Dagger Hilt, Retrofit / OkHttp, Android BLE GATT.
- **Двухканальный транспорт (Dual-Transport):** Управляет плеером либо через **Bluetooth Low Energy (BLE)**, либо через **Wi-Fi HTTP REST API** (переключается в репозитории на лету).
- **Экраны:**
  1. `PlayerScreen`: Управление треками (Play/Pause, Next, Prev, Seek бар, список плейлиста, выбор трека по клику, отображение текущего режима USB/External).
  2. `BleDiscoveryScreen`: Сканирование эфира, поиск устройства `ESP32_MP3_Player`, подключение/отключение BLE GATT.
  3. `WifiSettingsScreen`: Настройка точки доступа AP (SSID, пароль) и клиента STA (домашняя сеть Wi-Fi).
  4. `OtaScreen`: Выбор бинарных файлов `.bin` и прошивка Master ESP32 или Slave ESP32 по воздуху.
  5. `SettingsScreen`: Настройка тайм-аутов сна, уровней логирования компонентов.
  6. `SystemMonitorScreen`: Просмотр терминальных логов устройства в реальном времени, мониторинг памяти и перезагрузка.

---

## 2. АППАРАТНАЯ КАРТА ПОДКЛЮЧЕНИЙ И ПИНОВ

| Сигнал / Функция | Master ESP32-S3 | Slave ESP32 | Направление | Описание |
| :--- | :--- | :--- | :--- | :--- |
| **I2S Bit Clock (BCLK)** | `GPIO 41` (In) | `GPIO 26` (Out) | Slave ➔ Master | Тактовый сигнал битов аудио (44.1/48 kHz) |
| **I2S Word Select (WS)** | `GPIO 39` (In) | `GPIO 33` (Out) | Slave ➔ Master | Синхронизация каналов Left/Right |
| **I2S Data (DIN / DOUT)**| `GPIO 40` (In) | `GPIO 25` (Out) | Slave ➔ Master | Поток сэмплов PCM от Bluetooth к Master |
| **UART Link TX ➔ RX**    | `GPIO 8` (TX)   | `GPIO 22` (RX)  | Master ➔ Slave | Команды управления (0xAA) и OTA прошивка (0x55) |
| **UART Link RX ◄─ TX**    | `GPIO 9` (RX)   | `GPIO 23` (TX)  | Slave ➔ Master | Ответы ACK/NACK, статусы, версии ПО |
| **Sleep Sync (Линия сна)**| `GPIO 21` (Out) | `GPIO 34` (In)  | Master ➔ Slave | HIGH = работа, LOW = команда уйти в Deep Sleep |
| **Общая земля (GND)**    | `GND`           | `GND`           | Двунаправленно | Общая земля для стабильности I2S и UART |
| **Питание**              | `3.3V / 5V`     | `3.3V / 5V`     | Источник        | Стабилизированное питание |

### Внешняя периферия Master ESP32-S3:
- `GPIO 4`  — CAN TX (к микросхеме CAN-трансивера)
- `GPIO 15` — CAN RX (от CAN-трансивера; источник ext0 Wakeup)
- `GPIO 17` — I2S0 BCLK Out (к аудио ЦАП PCM5102)
- `GPIO 18` — I2S0 WS Out (к аудио ЦАП PCM5102)
- `GPIO 16` — I2S0 DOUT Out (к аудио ЦАП PCM5102)
- `GPIO 19` — USB OTG D- (шина данных USB-флешки)
- `GPIO 20` — USB OTG D+ (шина данных USB-флешки)
- `GPIO 0`  — Кнопка BOOT (ручное переключение / отладка)

---

## 3. ДЕТАЛЬНЫЕ ПРОТОКОЛЫ ВЗАИМОДЕЙСТВИЯ

### 3.1. Межчиповый протокол UART (115200 бод, 8N1)
Каждый пакет начинается с байта-префикса:
1. **Команды управления (Префикс `0xAA`):**
   - `0xAA 0x04` — `UART_CMD_NEXT_TRACK` ➔ Slave вызывает `periph_bluetooth_next()`.
   - `0xAA 0x05` — `UART_CMD_PREV_TRACK` ➔ Slave вызывает `periph_bluetooth_prev()`.
   - `0xAA 0x06` — `UART_CMD_GET_VERSION` ➔ Slave отвечает пакетом `version_response_t` (даты и время сборок компонентов).
2. **OTA-обновление Slave (Префикс `0x55`):**
   - Старт: `[0x55][0x01][size: 4B][crc32: 4B]` ➔ Slave инициализирует раздел `ota_0`/`ota_1` через `esp_ota_begin()`.
   - Блок данных: `[0x55][0x02][block_num: 2B][data_len: 1B][data: N байт (до 240)][crc16: 2B]` ➔ Slave проверяет CRC16 Modbus и пишет во flash.
   - Финализация: `[0x55][0x03][final_crc32: 4B]` ➔ Slave верифицирует общую CRC32, вызывает `esp_ota_set_boot_partition()` и перезагружается.

### 3.2. CAN-шина Ford Denso (TWAI)
- **Скорость:** 125 kbps, ID: `0x2D5`.
- **Логика команд:**
  * `data[1] & 0xF0 == 0x40` (кроме 0x49) — **NEXT TRACK**. В USB: следующий MP3. В External: шлет `0xAA 0x04` на Slave.
  * `data[1] & 0xF0 == 0x80` (кроме 0x89) — **PREV TRACK**. В USB: предыдущий MP3. В External: шлет `0xAA 0x05` на Slave.
  * `data[1] & 0xF0 == 0x10` (кроме 0x19) — **MODE SWITCH** (USB <-> BT).

### 3.3. Синхронизация энергосбережения и Deep Sleep
1. **Master:** при молчании CAN дольше `idle_timeout_sec` сохраняет позицию трека, опускает `GPIO 21` в **LOW (0V)**, включает пробуждение по CAN RX (`GPIO 15`, уровень LOW) и уходит в Deep Sleep.
2. **Slave:** задача `i2c_sleep_monitor_task` при появлении **LOW (0V)** на `GPIO 34` уходит в Deep Sleep, настраивая пробуждение по `GPIO 34`.
3. **Пробуждение:** CAN RX будит Master, Master поднимает `GPIO 21` в **HIGH (3.3V)**, что мгновенно будит Slave.

### 3.4. BLE GATT Протокол (Master ↔ Android App)
- **Service UUID:** `11000000-0000-1000-8000-00805f9b34fb`
- **Characteristics:**
  * `Command (11010000-...)`: Write (команды `play`, `pause`, `next`, `prev`, `track:<id>`, `seek:<pos_ms>`, `mode`).
  * `Status (11020000-...)`: Read / Notify (строка CSV: `track,position_ms,duration_ms,is_paused,playlist_size`).
  * `Playlist (11030000-...)`: Read / Notify (список файлов плейлиста).
  * `Logs (11040000-...)`: Read / Notify (текстовый поток системных логов).

### 3.5. HTTP REST API (Master ↔ Android App / Web UI)
- `GET /player_status` ➔ JSON статус плеера.
- `GET /playlist` ➔ JSON массив имен треков.
- `GET /player_cmd?cmd=...&track=...&pos=...` ➔ отправка команд.
- `GET/POST /wifi_settings` ➔ конфигурация AP / STA.
- `GET/POST /sleep_settings` ➔ конфигурация сна и тайм-аутов.
- `POST /upload` ➔ загрузка бинарника прошивки Master ESP32-S3.
- `POST /upload_slave` ➔ загрузка бинарника прошивки Slave ESP32.
- `GET /logs`, `POST /restart` ➔ утилиты диагностики.

---

## 4. СТРУКТУРА ПРОЕКТОВ В РЕПОЗИТОРИИ

```
D:\emulators\
├── Ford-DensO-mp3-master\              <-- Проект MASTER ESP32-S3
│   ├── main\
│   │   ├── play_music-master.c         <-- Основной монолит (4756 строк)
│   │   ├── uart_ota_master.c           <-- UART протокол Master
│   │   ├── web_ota.c                   <-- HTTP веб-сервер и Web OTA
│   │   └── ble_player_service.h        <-- Заголовки BLE
│   ├── components\
│   │   ├── ble_player_service\         <-- BLE GATT сервис
│   │   ├── elm327_wifi\                <-- TCP эмулятор ELM327
│   │   ├── wifi_can_sniffer\           <-- UDP транслятор CAN
│   │   └── my_board\                   <-- Пины I2S и кодека
│   └── AndroidPlayer\                  <-- ANDROID ПРИЛОЖЕНИЕ (Kotlin Compose)
│       └── app\src\main\java\com\example\androidplayer\
│           ├── data\ble\               <-- BlePlayerClient.kt (GATT клиент)
│           ├── data\network\           <-- Esp32PlayerApi.kt (Retrofit REST)
│           ├── data\repository\        <-- Esp32PlayerRepository.kt (Dual-Transport)
│           └── ui\screens\             <-- Экраны: Player, BLE, Wifi, OTA, Logs
│
└── Ford-DensO-bluetooth-slave\         <-- Проект SLAVE ESP32
    └── main\
        ├── a2dp-slave.c                <-- Ядро Slave (Bluetooth A2DP, авто-реконнект)
        ├── uart_ota_slave.c            <-- Запись OTA прошивки по UART во Flash
        ├── uart_ota_crc.c              <-- Алгоритмы CRC16/CRC32
        └── uart_ota_protocol.h         <-- Описание пакетов межчиповой связи
```

---

## 5. ВЫЯВЛЕННЫЕ ПРОБЛЕМЫ И ЦЕЛИ РЕФАКТОРИНГА

### Master ESP32-S3:
1. **Монолитность `play_music-master.c`:** 4756 строк в одном файле. Требуется разделение на модули: `app_player`, `app_can`, `app_power`, `app_usb`, `app_ble`, `app_slave_link`.
2. **Заглушки в BLE:** в `ble_player_service.c` нет реального периодического таймера `Notify`, а на запросы плейлиста возвращается статический хардкод.
3. **Отсутствие регулятора громкости:** ЦАП PCM5102 не имеет аппаратного I2C регулятора, требуется программный софтверный регулятор громкости (Software Volume).
4. **Неточный Seek в VBR MP3:** расчет смещения по фиксированному битрейту сбивает перемотку.

### Slave ESP32:
1. **Отсутствие передачи метаданных:** Slave знает название песни и артиста от смартфона по AVRCP, но не передает их на Master по UART.
2. **Синхронизация паузы Bluetooth:** при переключении магнитолы на USB нужно слать `PAUSE` на смартфон через AVRCP.

### Android-приложение (`AndroidPlayer`):
1. **Блокировка `switch_mode` по BLE:** в `Esp32PlayerRepository.kt` выброс исключения `throw UnsupportedOperationException("switch_mode not supported over BLE")`, хотя по BLE переключение режимов реализовать очень просто.
2. **Разрыв BLE-нотификаций:** из-за отсутствия таймера в прошивке экран плеера в приложении не обновляет ползунок времени при подключении по BLE.

---

## 6. ЧЕК-ЛИСТ ТРЕБОВАНИЙ ДЛЯ РАЗРАБОТКИ НОВОГО КОДА

> Отметьте нужные пункты `[x]`, скорректируйте или допишите свои пожелания.

```text
[ ] 1. АРХИТЕКТУРА И МОДУЛЬНОСТЬ (MASTER):
    - [ ] Разбить play_music-master.c на независимые модули:
          * main/app_main.c (инициализация и общая координация)
          * main/player/audio_player.c (.h) (управление USB и воспроизведением)
          * main/can/can_manager.c (.h) (TWAI CAN и кнопки Ford 0x2D5)
          * main/power/power_manager.c (.h) (мониторинг шины, Deep Sleep, GPIO 21)
          * main/slave_link/slave_link.c (.h) (UART протокол и OTA прошивка Slave)
          * main/ble/ble_manager.c (.h) (рабочий BLE сервер)

[ ] 2. АРХИТЕКТУРА И МОДУЛЬНОСТЬ (SLAVE):
    - [ ] Рефакторинг a2dp-slave.c:
          * Вынести Bluetooth стек A2DP в отдельный модуль
          * Вынести UART команды и OTA в отдельный модуль
          * Очистить устаревший код HFP

[ ] 3. ВЗАИМОДЕЙСТВИЕ MASTER <-> SLAVE:
    - [ ] Добавить передачу метаданных трека (Название, Артист) со Slave на Master по UART
    - [ ] Автоматическая пауза Bluetooth на смартфоне при переключении источника на USB
    - [ ] Синхронизация уровня громкости между Master и Slave

[ ] 4. АУДИОСИСТЕМА:
    - [ ] Реализовать программную регулировку громкости (Software Volume) для ЦАП PCM5102
    - [ ] Улучшить точность Seek (перемотки) для треков с переменным битрейтом (VBR)
    - [ ] Добавить навигацию по папкам флешки (папка как альбом)

[ ] 5. ANDROID-ПРИЛОЖЕНИЕ (AndroidPlayer):
    - [ ] Включить команду переключения режима USB/BT (switch_mode) через BLE
    - [ ] Обеспечить плавное обновление позиции воспроизведения (Seek Bar) по BLE Notify
    - [ ] Отображать название трека и исполнителя при воспроизведении по Bluetooth
```
