# ПОВНЕ ТЕХНІЧНЕ ЗАВДАННЯ ТА АРХІТЕКТУРНИЙ ПЛАН

# Медіасистема Ford Denso: Master (ESP32-S3) + Slave (ESP32 BT) + Android App

> **Призначення документа:** повний технічний опис апаратної частини, архітектури, протоколів взаємодії та покрокових алгоритмів роботи **всієї екосистеми**: Master ESP32-S3, Slave ESP32 і Android-застосунку (`AndroidPlayer`). Документ самодостатній і може бути переданий будь-якому AI-агенту або розробнику для створення нової модульної кодової бази.

---

## 1. ЗАГАЛЬНИЙ ОГЛЯД І РОЛІ КОМПОНЕНТІВ

Система емулює штатний медіаплеєр для головних пристроїв автомобілів Ford (з шиною CAN магнітол Ford Denso / Visteon) і розподілена між трьома ключовими вузлами:

```
[ Смартфон: Музика (A2DP) ]
        │
        ▼
[ ESP32 SLAVE BT ] (A2DP-приймач "ForD DensO")
        │
        ├─── I2S Звук (GPIO 26, 33, 25) ─────► I2S Вхід (GPIO 41, 39, 40) ────┐
        ├─── UART Команди/OTA (GPIO 22, 23) ◄──► UART Зв'язок (GPIO 8, 9) ────┤
        └─── Sleep Sync Вхід (GPIO 34) ◄────── Sleep Sync Вихід (GPIO 21) ──┤
                                                                            ▼
                                                                 [ ESP32-S3 MASTER ]
                                                                    │   │   │
                      CAN-шина Ford (GPIO 4 TX, 15 RX) ─────────────┘   │   └── USB Flash OTG (GPIO 19, 20)
                      I2S Вихід на ЦАП PCM5102 (GPIO 16, 17, 18) ───────┘
                                                                            │
                                                        ┌───────────────────┴───────────────────┐
                                                        ▼ (BLE GATT)                     ▼ (Wi-Fi HTTP — лише браузер)
                                            [ ANDROID-ЗАСТОСУНОК: AndroidPlayer (Jetpack Compose) ]
```

### Роль 1: Master ESP32-S3

- **Сховище і плеєр:** читає USB-флешку (FAT32) через USB-Host OTG. Відтворює аудіо (MP3) через пайплайн ESP-ADF.
- **ЦАП (вихід звуку):** тактує зовнішній аудіо-ЦАП PCM5102 по I2S0 Master (44.1/48 кГц, 16 біт стерео).
- **Вхід зовнішнього звуку:** приймає потік I2S1 Slave від ESP32 Slave і транслює його в ЦАП у режимі passthrough.
- **CAN-шина Ford:** підключається до CAN-шини (125 кбіт/с, ID `0x2D5`), обробляє кнопки керма/магнітоли, фільтрує брязкіт (debounce).
- **Синхронізація живлення:** за відсутності CAN іде в Deep Sleep і сигналом через `GPIO 21` відправляє Slave-чіп у Deep Sleep. Прокидається від CAN RX (`GPIO 15`).
- **Зв'язок і OTA зі Slave:** передає команди Next/Prev по UART1 і виконує поблочну OTA-прошивку плати Slave.
- **Мережеві сервіси:** Wi-Fi SoftAP/STA, панель Web UI, Web OTA, UDP CAN Sniffer (wxCAN-Sniffer), TCP-сервер ELM327 (діагностика), BLE GATT-сервер для мобільного застосунку.

### Роль 2: Slave ESP32 (Bluetooth Audio Sink)

- **Стек Bluetooth:** A2DP Sink (ім'я `"ForD DensO"`) і профіль керування AVRCP.
- **Автоперепідключення (Auto-reconnect):** зберігає MAC-адресу останнього підключеного смартфона в NVS (`a2dp_sink`). Під час старту фонова задача `bt_reconnect_task` автоматично відновлює з'єднання.
- **Аудіовихід:** передає декодований PCM-звук по апаратному I2S0 (Master TX) на Master ESP32-S3.
- **Обробник UART-команд (`uart_cmd_task`):**
  - приймає звичайні команди керування (`0xAA 0x04` Next, `0xAA 0x05` Prev) і перемикає треки на смартфоні через AVRCP;
  - приймає запит версії (`0xAA 0x06`) і повертає структуру версій збірок;
  - приймає потік OTA-оновлення (`0x55 …`) і прошиває свій Flash-розділ (`ota_0`/`ota_1`) з перевіркою CRC16/CRC32.
- **Контроль живлення:** задача `i2c_sleep_monitor_task` опитує `GPIO 34`. Щойно Master переводить лінію в LOW, Slave іде в Deep Sleep. Прокидається за рівнем на `GPIO 34`.

### Роль 3: Android-застосунок (`AndroidPlayer`)

- **Стек:** Kotlin, Jetpack Compose, Material 3, Coroutines, StateFlow, Dagger Hilt, Retrofit / OkHttp, Android BLE GATT.
- **Транспорт — тільки BLE.** Увесь обмін із Master іде через **Bluetooth Low Energy (BLE)**: JSON-RPC + бінарні кадри OTA. Wi-Fi HTTP REST на пристрої є, але ним користується **лише веб-браузер** (Web UI), застосунок його не використовує.
- **Екрани:**
  1. `PlayerScreen`: керування треками (Play/Pause, Next, Prev, смуга Seek, список плейлиста, вибір треку кліком, відображення поточного режиму USB/External).
  2. `BleDiscoveryScreen`: сканування ефіру, пошук пристрою `ESP32_MP3_Player`, підключення/відключення BLE GATT.
  3. `WifiSettingsScreen`: налаштування точки доступу AP (SSID, пароль) і клієнта STA (домашня мережа Wi-Fi).
  4. `OtaScreen`: вибір бінарних файлів `.bin` і прошивка Master ESP32 або Slave ESP32 по повітрю.
  5. `SettingsScreen`: налаштування тайм-аутів сну, рівнів логування компонентів.
  6. `SystemMonitorScreen`: перегляд термінальних логів пристрою в реальному часі, моніторинг пам'яті й перезавантаження.

---

## 2. АПАРАТНА КАРТА ПІДКЛЮЧЕНЬ І ПІНІВ

| Сигнал / функція | Master ESP32-S3 | Slave ESP32 | Напрямок | Опис |
| :--- | :--- | :--- | :--- | :--- |
| **I2S Bit Clock (BCLK)** | `GPIO 41` (In) | `GPIO 26` (Out) | Slave ➔ Master | Тактовий сигнал бітів аудіо (44.1/48 кГц) |
| **I2S Word Select (WS)** | `GPIO 39` (In) | `GPIO 33` (Out) | Slave ➔ Master | Синхронізація каналів Left/Right |
| **I2S Data (DIN / DOUT)** | `GPIO 40` (In) | `GPIO 25` (Out) | Slave ➔ Master | Потік PCM-семплів від Bluetooth до Master |
| **UART Link TX ➔ RX** | `GPIO 8` (TX) | `GPIO 22` (RX) | Master ➔ Slave | Команди керування (0xAA) і OTA-прошивка (0x55) |
| **UART Link RX ◄─ TX** | `GPIO 9` (RX) | `GPIO 23` (TX) | Slave ➔ Master | Відповіді ACK/NACK, статуси, версії ПЗ |
| **Sleep Sync (лінія сну)** | `GPIO 21` (Out) | `GPIO 34` (In) | Master ➔ Slave | HIGH = робота, LOW = команда йти в Deep Sleep |
| **Спільна земля (GND)** | `GND` | `GND` | Двонаправлено | Спільна земля для стабільності I2S і UART |
| **Живлення** | `3.3V / 5V` | `3.3V / 5V` | Джерело | Стабілізоване живлення |

### Зовнішня периферія Master ESP32-S3:

- `GPIO 4` — CAN TX (до мікросхеми CAN-трансивера)
- `GPIO 15` — CAN RX (від CAN-трансивера; джерело ext0 Wakeup)
- `GPIO 17` — I2S0 BCLK Out (до аудіо-ЦАП PCM5102)
- `GPIO 18` — I2S0 WS Out (до аудіо-ЦАП PCM5102)
- `GPIO 16` — I2S0 DOUT Out (до аудіо-ЦАП PCM5102)
- `GPIO 19` — USB OTG D- (шина даних USB-флешки)
- `GPIO 20` — USB OTG D+ (шина даних USB-флешки)
- `GPIO 0` — кнопка BOOT (ручне перемикання / налагодження)

---

## 3. ДЕТАЛЬНІ ПРОТОКОЛИ ВЗАЄМОДІЇ

### 3.1. Міжчіповий протокол UART (115200 бод, 8N1)

Кожен пакет починається з байта-префікса:

1. **Команди керування (префікс `0xAA`):**
   - `0xAA 0x04` — `UART_CMD_NEXT_TRACK` ➔ Slave викликає `periph_bluetooth_next()`.
   - `0xAA 0x05` — `UART_CMD_PREV_TRACK` ➔ Slave викликає `periph_bluetooth_prev()`.
   - `0xAA 0x06` — `UART_CMD_GET_VERSION` ➔ Slave відповідає пакетом `version_response_t` (дати й час збірок компонентів).
2. **OTA-оновлення Slave (префікс `0x55`):**
   - Старт: `[0x55][0x01][size: 4B][crc32: 4B]` ➔ Slave ініціалізує розділ `ota_0`/`ota_1` через `esp_ota_begin()`.
   - Блок даних: `[0x55][0x02][block_num: 2B][data_len: 1B][data: N байт (до 240)][crc16: 2B]` ➔ Slave перевіряє CRC16 Modbus і пише у flash.
   - Фіналізація: `[0x55][0x03][final_crc32: 4B]` ➔ Slave верифікує загальну CRC32, викликає `esp_ota_set_boot_partition()` і перезавантажується.

### 3.2. CAN-шина Ford Denso (TWAI)

- **Швидкість:** 125 кбіт/с, ID: `0x2D5`.
- **Логіка команд:**
  * `data[1] & 0xF0 == 0x40` (крім 0x49) — **NEXT TRACK**. У USB: наступний MP3. В External: шле `0xAA 0x04` на Slave.
  * `data[1] & 0xF0 == 0x80` (крім 0x89) — **PREV TRACK**. У USB: попередній MP3. В External: шле `0xAA 0x05` на Slave.
  * `data[1] & 0xF0 == 0x10` (крім 0x19) — **MODE SWITCH** (USB <-> BT).

### 3.3. Синхронізація енергозбереження та Deep Sleep

1. **Master:** при тиші на CAN довше за `idle_timeout_sec` зберігає позицію треку, опускає `GPIO 21` у **LOW (0V)**, вмикає пробудження по CAN RX (`GPIO 15`, рівень LOW) і йде в Deep Sleep.
2. **Slave:** задача `i2c_sleep_monitor_task` при появі **LOW (0V)** на `GPIO 34` іде в Deep Sleep, налаштовуючи пробудження по `GPIO 34`.
3. **Пробудження:** CAN RX будить Master, Master піднімає `GPIO 21` у **HIGH (3.3V)**, що миттєво будить Slave.

### 3.4. BLE GATT протокол (Master ↔ Android App)

- **Service UUID:** `11000000-0000-1000-8000-00805f9b34fb`
- **Характеристики:**
  * `Command (11010100-0000-1000-8000-00805f9b34fb)`: Write (команди `play`, `pause`, `next`, `prev`, `track:<id>`, `seek:<pos_ms>`, `mode`).
  * `Status (11010200-0000-1000-8000-00805f9b34fb)`: Read / Notify (рядок CSV: `track,position_ms,duration_ms,is_paused,playlist_size,mode,sniffer`).
  * `Playlist (11010300-0000-1000-8000-00805f9b34fb)`: Read / Notify (список файлів плейлиста).
  * `Logs (11010400-0000-1000-8000-00805f9b34fb)`: Read / Notify (текстовий потік системних логів).

### 3.5. HTTP REST API (Master ↔ Android App / Web UI)

- `GET /player_status` ➔ JSON-статус плеєра.
- `GET /playlist` ➔ JSON-масив назв треків.
- `GET /player_cmd?cmd=…&track=…&pos=…` ➔ надсилання команд.
- `GET/POST /wifi_settings` ➔ конфігурація AP / STA.
- `GET/POST /sleep_settings` ➔ конфігурація сну й тайм-аутів.
- `POST /upload` ➔ завантаження бінарника прошивки Master ESP32-S3.
- `POST /upload_slave` ➔ завантаження бінарника прошивки Slave ESP32.
- `GET /logs`, `POST /restart` ➔ утиліти діагностики.

---

## 4. СТРУКТУРА ПРОЄКТІВ У РЕПОЗИТОРІЇ

```
D:\emulators\
├── Ford-DensO-mp3-master\              <-- Проєкт MASTER ESP32-S3
│   ├── main\
│   │   ├── player_master.c             <-- Основний модуль (запуск, CAN, USB, BLE, Web)
│   │   ├── uart_ota_master.c           <-- UART-протокол Master
│   │   ├── web_ota.c                   <-- HTTP веб-сервер і Web OTA
│   │   └── player_ble.c                <-- BLE GATT / RPC
│   ├── components\
│   │   ├── ble_player_service\         <-- BLE GATT-сервіс
│   │   ├── elm327_wifi\                <-- TCP-емулятор ELM327
│   │   ├── wifi_can_sniffer\           <-- UDP-транслятор CAN
│   │   └── my_board\                   <-- Піни I2S і кодека
│   └── AndroidPlayer\                  <-- ANDROID-ЗАСТОСУНОК (Kotlin Compose)
│       └── app\src\main\java\com\example\androidplayer\
│           ├── data\ble\               <-- BlePlayerClient.kt (GATT-клієнт)
│           ├── data\network\           <-- Esp32PlayerApi.kt (Retrofit REST)
│           ├── data\repository\        <-- Esp32PlayerRepository.kt (BLE-only)
│           └── ui\screens\             <-- Екрани: Player, BLE, Wifi, OTA, Logs
│
└── Ford-DensO-bluetooth-slave\         <-- Проєкт SLAVE ESP32
    └── main\
        ├── a2dp-slave.c                <-- Ядро Slave (Bluetooth A2DP, автоперепідключення)
        ├── avrc_media.c                <-- AVRCP-метадані (назва, виконавець, позиція)
        ├── uart_ota_slave.c            <-- Запис OTA-прошивки по UART у Flash
        ├── uart_ota_crc.c              <-- Алгоритми CRC16/CRC32
        └── uart_ota_protocol.h         <-- Опис пакетів міжчіпового зв'язку
```

---

## 5. ВИЯВЛЕНІ ПРОБЛЕМИ ТА ЦІЛІ РЕФАКТОРИНГУ

> Стан на момент написання документа. Частину пунктів уже закрито (див. `HELP.md`).

### Master ESP32-S3:

1. **Монолітність `player_master.c`:** багато рядків в одному файлі. Потрібне розділення на модулі: `app_player`, `app_can`, `app_power`, `app_usb`, `app_ble`, `app_slave_link`.
2. **Заглушки в BLE:** історично не було реального періодичного таймера `Notify`, а на запити плейлиста повертався статичний хардкод.
3. **Відсутність регулятора громкості:** ЦАП PCM5102 не має апаратного I2C-регулятора, потрібен програмний регулятор громкості (Software Volume).
4. **Неточний Seek у VBR MP3:** розрахунок зміщення за фіксованим бітрейтом збиває перемотку.

### Slave ESP32:

1. **Відсутність передачі метаданих:** Slave знає назву пісні й виконавця від смартфона по AVRCP, але не передає їх на Master по UART. *(закрито у v1.0.5)*
2. **Синхронізація паузи Bluetooth:** при перемиканні магнітоли на USB потрібно слати `PAUSE` на смартфон через AVRCP.

### Android-застосунок (`AndroidPlayer`):

1. **Блокування `switch_mode` по BLE:** в `Esp32PlayerRepository.kt` було викидання винятку `throw UnsupportedOperationException("switch_mode not supported over BLE")`.
2. **Розрив BLE-нотифікацій:** через відсутність таймера в прошивці екран плеєра не оновлював смугу часу при підключенні по BLE.

---

## 6. ЧЕК-ЛИСТ ВИМОГ ДЛЯ РОЗРОБКИ НОВОГО КОДУ

> Позначте потрібні пункти `[x]`, скоригуйте або допишіть свої побажання.

```text
[ ] 1. АРХІТЕКТУРА І МОДУЛЬНІСТЬ (MASTER):
    - [ ] Розбити player_master.c на незалежні модулі:
          * main/app_main.c (ініціалізація та загальна координація)
          * main/player/audio_player.c (.h) (керування USB і відтворенням)
          * main/can/can_manager.c (.h) (TWAI CAN і кнопки Ford 0x2D5)
          * main/power/power_manager.c (.h) (моніторинг шини, Deep Sleep, GPIO 21)
          * main/slave_link/slave_link.c (.h) (UART-протокол і OTA-прошивка Slave)
          * main/ble/ble_manager.c (.h) (робочий BLE-сервер)

[ ] 2. АРХІТЕКТУРА І МОДУЛЬНІСТЬ (SLAVE):
    - [ ] Рефакторинг a2dp-slave.c:
          * Винести стек Bluetooth A2DP в окремий модуль
          * Винести UART-команди й OTA в окремий модуль
          * Прибрати застарілий код HFP

[ ] 3. ВЗАЄМОДІЯ MASTER <-> SLAVE:
    - [ ] Додати передачу метаданих треку (назва, виконавець) зі Slave на Master по UART
    - [ ] Автоматична пауза Bluetooth на смартфоні при перемиканні джерела на USB
    - [ ] Синхронізація рівня громкості між Master і Slave

[ ] 4. АУДІОСИСТЕМА:
    - [ ] Реалізувати програмне регулювання громкості (Software Volume) для ЦАП PCM5102
    - [ ] Покращити точність Seek (перемотки) для треків зі змінним бітрейтом (VBR)
    - [ ] Додати навігацію по папках флешки (папка як альбом)

[ ] 5. ANDROID-ЗАСТОСУНОК (AndroidPlayer):
    - [ ] Увімкнути команду перемикання режиму USB/BT (switch_mode) через BLE
    - [ ] Забезпечити плавне оновлення позиції відтворення (Seek Bar) по BLE Notify
    - [ ] Відображати назву треку й виконавця при відтворенні по Bluetooth
```
