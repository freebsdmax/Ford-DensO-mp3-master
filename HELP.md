# Ford Player — довідка (HELP)

Один аудіо-комплекс з трьох частин, які працюють разом. Цей файл — точка входу:
що де лежить, як зібрати, як прошити, як оновлювати і що робити, коли щось не так.

## Три репозиторії (пов'язані між собою)

| Компонент | Репозиторій | Роль |
|---|---|---|
| **Master** (цей) | https://github.com/freebsdmax/Ford-DensO-mp3-master | ESP32-S3: MP3 з USB, BLE-сервер, Web UI, OTA, CAN, керує Slave по UART |
| **Slave** | https://github.com/freebsdmax/Ford-DensO-bluetooth-slave | ESP32: Bluetooth A2DP-приймач (телефон → I2S), AVRCP-метадані, UART-команди від Master |
| **App** | https://github.com/freebsdmax/Esp32_Android_app | Android (BLE-only): плеєр, плейлист, OTA, логи, моніторинг |

Дзеркальні довідки:

- Slave → `HELP.md` у https://github.com/freebsdmax/Ford-DensO-bluetooth-slave
- App → `HELP.md` у https://github.com/freebsdmax/Esp32_Android_app

Сумісність: Master `v1.0.10+` ↔ Slave `v1.0.6+` ↔ App `v1.0.13+`.
Старіші версії працюють, але без метаданих Bluetooth і без оновлення «через /storage».

## Як воно з'єднане

```
   Телефон ──Bluetooth (A2DP/AVRCP)──► SLAVE ──I2S (BCLK 41 / WS 39 / DIN 40)──┐
                                        │                                       │
                                        │ UART1 @115200 (TX 8 / RX 9)           │
                                        ▼                                       ▼
                                     MASTER (ESP32-S3) ── I2S out (BCLK 17 / WS 18 / DOUT 16) ──► DAC
                                        ▲
                    BLE GATT (JSON-RPC) │ Wi-Fi (HTTP + Web UI)
                                        │
                                     APP / браузер
```

- **BLE** — тільки для застосунку (керування, статус, OTA).
- **Wi-Fi** — Web UI та OTA по HTTP (`192.168.88.x` у STA або `192.168.4.1` в AP).
- **UART 115200** — Master ↔ Slave: команди, версії, метадані, прошивка Slave.
  Швидкість **не підвищувати** — на 115200 лінія стабільна.

## Апаратна частина

| Параметр | Значення |
|---|---|
| MCU | ESP32-S3 (N16R8) |
| Flash / PSRAM | 16 МБ / 8 МБ Octal @ 80 МГц |
| Framework | ESP-IDF **v5.5.1** + ESP-ADF (`fcce23e3`) |
| UART до Slave | UART1, TX 8, RX 9, **115200** |
| I2S вхід від Slave | BCLK 41, WS 39, DIN 40 |
| I2S вихід на DAC | BCLK 17, WS 18, DOUT 16 |
| USB | USB Host (MSC) — флешка з MP3 |

## Розділи flash (16 МБ)

| Name | Type | Offset | Size | Призначення |
|---|---|---|---|---|
| nvs | data | 0x9000 | 0x6000 | налаштування |
| otadata | data | 0xF000 | 0x2000 | вибір OTA-слота |
| factory | app | 0x20000 | 3 МБ | заводська прошивка |
| ota_0 | app | 0x320000 | 3 МБ | OTA-слот 0 |
| ota_1 | app | 0x620000 | 3 МБ | OTA-слот 1 |
| storage | data/fat | 0x920000 | **4.9 МБ** | FAT `/storage` — сюди лягає .bin до прошивки |
| staging | data 0x40 | 0xE00000 | 2 МБ | тимчасовий образ для UART-OTA |

## Збірка і прошивка (USB)

```powershell
cd D:\emulators\Ford-DensO-mp3-master
& "C:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1"
idf.py build
idf.py -p COMx flash monitor
```

> ESP-ADF має бути в `ADF_PATH` (`C:\Espressif\esp-adf`). Патч ESP-ADF для FreeRTOS
> (`idf_patches/idf_v5.5_freertos.patch`) локально вже застосований; **у CI він
> застосовується кроком workflow** — без нього декодер не створюється і всі MP3
> пропускаються з `Not found right xTaskCreateRestrictedPinnedToCore`.

Злитий образ для `0x0` (16 МБ):

```powershell
python -m esptool merge_bin --flash_mode dio --flash_freq 80m --flash_size 16MB `
  0x0 build\bootloader\bootloader.bin `
  0x8000 build\partition_table\partition-table.bin `
  0xf000 build\ota_data_initial.bin `
  0x20000 build\Ford-DensO-mp3-master.bin
```

## Оновлення прошивки — три шляхи

### 1. Через застосунок (BLE) — основний

```
1) качає .bin з GitHub Release
2) заливає його у /storage/master.bin (або /storage/slave.bin)
3) просить плату прошитись з цього файлу
4) плата прошиває → і лише після успіху видаляє файл
```

- обірвався BLE — файл лишається, повторна спроба не потребує інтернету;
- прогрес і результат — у шторці (foreground service `OtaUploadService`);
- Master після прошивки перезавантажується, застосунок сам перепідключається.

RPC-послідовність: `file.begin{size,target:"storage",name}` → `file.data{offset,data}`
(binary frames) → `file.finish` → `file.flash{name,target}` → `file.status`.

### 2. Web UI (HTTP)

```
POST /upload             — образ Master (одразу в OTA-слот, перезавантаження)
POST /upload_slave       — образ Slave (Master стримить по UART)
POST /upload_to_storage  — образ у /storage/firmware.bin (без прошивки)
```

```powershell
curl.exe -X POST --data-binary "@build/Ford-DensO-mp3-master.bin" `
  -H "Content-Type: application/octet-stream" http://<ip>/upload
```

> `Invoke-WebRequest` не вміє chunked upload — використовуй `curl.exe`.

### 3. USB (аварійний)

`idf.py -p COMx flash` або `Ford-DensO-mp3-master-full.bin` з релізу на `0x0`.

### Оновлення Slave

- з застосунку — кнопка в картці оновлень (BLE → Master → UART-OTA, ~2 хв);
- з Web UI — `POST /upload_slave`;
- вручну — Налаштування → «Прошивка вручну» в застосунку.

## BLE API (GATT, JSON-RPC)

Сервіс `11000000-0000-1000-8000-00805f9b34fb`:

| Характеристика | UUID | Призначення |
|---|---|---|
| Command | `…-11010100-…` | запити (write) |
| Status | `…-11010200-…` | `track,pos,dur,paused,count,mode,sniffer` |
| Playlist | `…-11010300-…` | назви треків |
| Logs | `…-11010400-…` | відповіді RPC |

Формат запиту: `rpc:<метод>` або `rpc:<метод>:<json>`.

| Метод | Опис |
|---|---|
| `system_info.get` | heap/PSRAM/uptime/версія |
| `version.get` | версії Master/Slave/Web/UART + `slave_fw_version` |
| `media.get` | що грає на телефоні: `valid,playing,pos_ms,dur_ms,title,artist` |
| `playlist.get` | `{offset,count}` → `{total,items,next}` |
| `file.begin/data/finish/flash/abort/status` | OTA (у т.ч. через `/storage`) |
| `slave_ota.*` | UART-OTA Slave (старий шлях) |
| `ota.*` | OTA Master у RAM (старий шлях) |
| `sleep.get/set` | таймаут у **секундах** (10–28800) і режим сну |
| `wifi.get/set` | STA/AP |
| `sniffer.set` | CAN-сніфер ON/OFF |
| `can_statistics.get` | кадри/байти/бітрейт |
| `logs.get`, `log_level.*`, `verbose.*`, `component_logs.*` | логи |
| `device.restart` | перезавантаження |

Керування плеєром (`sendCommand`, без RPC): `play`, `pause`, `next`, `prev`, `seek`.

## UART протокол (Master ↔ Slave)

Звичайні команди: `[0xAA][cmd]`; OTA — `[0x55][...]`.

| Код | Напрям | Значення |
|---|---|---|
| `0x04` | M→S | наступний трек |
| `0x05` | M→S | попередній трек |
| `0x06` | M→S | версія збірки (NVS) |
| `0x07` | M→S | play (AVRCP) |
| `0x08` | M→S | pause (AVRCP) |
| `0x09` | M→S | версія прошивки → `[0xAA][0x09][len][chars]` |
| `0x0C` | M→S | дай стан медіа |
| `0x0A` | S→M | стан медіа `flags,pos:4,dur:4,tlen,title,alen,artist` |
| `0x0B` | M→S | seek `[delta_ms:int32 LE]` (наближений) |

Доступ до UART серіалізований м'ютексом `uart_link_lock()`; під час OTA
медіа/версії не опитуються (`uart_link_ota_active()`).

## Медіа з телефона (AVRCP)

Slave — AVRCP **CT**, телефон — **TG**. Slave підписується на
`TRACK_CHANGE` / `PLAY_STATUS_CHANGE` / `PLAY_POS_CHANGED`, питає
`GetElementAttributes` (TITLE, ARTIST, PLAYING_TIME) і раз на 1.5 с —
`GetPlayStatus`. Далі Master опитує Slave раз на секунду і віддає в
`media.get` та `/player_status`.

Що дає YouTube: заголовок відео, назву каналу, тривалість, позицію, play/pause.
Альбом зазвичай порожній, у live-стрімів тривалість 0.

**Перемотка**: AVRCP не має абсолютного seek, тому використовуються натискання
⏩/⏪ (≈10 с кожне, максимум 3 за жест, дельта < 5 с ігнорується).

## Web UI

| Endpoint | Опис |
|---|---|
| `/` | сторінка плеєра |
| `/player_status` | статус + `media_*` поля |
| `/player_cmd?cmd=…` | play/pause/next/prev/set_pos/set_sniffer/switch_mode |
| `/playlist` | список треків |
| `/version`, `/ota_status` | версії та прогрес OTA |
| `/logs`, `/log_level`, `/verbose_logs`, `/component_logs` | логи |
| `/wifi_settings`, `/sleep_settings` | налаштування |
| `/upload`, `/upload_slave`, `/upload_to_storage` | OTA |
| `/restart` | перезавантаження |
| `/elm327_status`, `/elm327_config` | емулятор ELM327 |

## Логи

5 груп: **плеєр**, **зв'язок** (BLE/Wi-Fi/UART), **OTA**, **авто** (CAN/ELM327),
**система** (сон/NVS/USB). Групи вмикаються/вимикаються з застосунку
(`component_logs.set`), логи пишуться в кільцевий буфер і віддаються в `/logs`.
`verbose.*` додає емодзі та банери.

## Релізи та CI

- кожен репозиторій публікує **свій** GitHub Release на тег `v*`;
- Master: `Ford-DensO-mp3-master.bin` (+ `-full.bin`, bootloader, table, ota_data);
- Slave: `Ford-DensO-bluetooth-slave.bin` (+ те саме);
- App: `Ford-Player-vX.Y.Z.apk` (підписаний, `versionName` з тега).

```powershell
git tag v1.0.10
git push origin v1.0.10
```

Секрети для APK: `KEYSTORE_BASE64`, `KEYSTORE_PASSWORD`, `KEY_ALIAS`, `KEY_PASSWORD`.
`minifyEnabled false` — Gson/Retrofit працюють через рефлексію.

## Що робити, якщо…

| Симптом | Причина / дія |
|---|---|
| Усі MP3 пропускаються, `Not found right xTaskCreateRestrictedPinnedToCore` | не застосований `idf_v5.5_freertos.patch` до ESP-IDF |
| `Не вдалося перевірити релізи GitHub` | ліміт 60 запитів/год на IP; застосунок показує кеш і час скидання |
| Застосунок «підключено», але пусто | застарілий GATT: кнопка «Оновити» в Налаштуваннях (або автовідновлення) |
| `COM6: Access is denied` | порт зайнятий іншим монітором/idf.py — закрити |
| Прошивка впала | образ лишається в `/storage`, повторна спроба без інтернету |
| Slave не оновлюється | Slave старіший за `v1.0.3` — раз прошити по USB |

## Корисні посилання

- Master: https://github.com/freebsdmax/Ford-DensO-mp3-master
- Slave: https://github.com/freebsdmax/Ford-DensO-bluetooth-slave
- App: https://github.com/freebsdmax/Esp32_Android_app
- Релізи Master: https://github.com/freebsdmax/Ford-DensO-mp3-master/releases
- Релізи Slave: https://github.com/freebsdmax/Ford-DensO-bluetooth-slave/releases
- Релізи App: https://github.com/freebsdmax/Esp32_Android_app/releases
