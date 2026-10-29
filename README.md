# Ford Player — Master (ESP32-S3)

Прошивка головного контролера аудіосистеми: плеєр з USB-флешки, керування по BLE,
веб-інтерфейс, OTA (по Wi-Fi, BLE та UART) і трансляція аудіо з Bluetooth-модуля (Slave).

Система складається з трьох частин:

| Компонент | Репозиторій | Роль |
|---|---|---|
| **Master** | цей репозиторій | USB/MP3 плеєр, BLE + Web UI, OTA-сервер для Slave |
| **Slave** | `Ford-DensO-bluetooth-slave` | ESP32 A2DP/HFP-приймач, віддає звук по I2S, керується по UART |
| **App** | `AndroidPlayer/` | Android-застосунок (BLE-only): плеєр, плейлист, OTA, логи |


## Довідка та пов'язані репозиторії

| Компонент | Репозиторій |
|---|---|
| Master (ESP32-S3) | https://github.com/freebsdmax/Ford-DensO-mp3-master |
| Slave (ESP32 Bluetooth) | https://github.com/freebsdmax/Ford-DensO-bluetooth-slave |
| Android-застосунок | https://github.com/freebsdmax/Esp32_Android_app |

- Повна довідка по системі: [HELP.md](https://github.com/freebsdmax/Ford-DensO-mp3-master/blob/main/HELP.md)
- Довідка цього компонента: [HELP.md](HELP.md)

---

## Залізо

| Параметр | Значення |
|---|---|
| Чіп | **ESP32-S3** (N16R8) |
| Flash | 16 МБ |
| PSRAM | 8 МБ, **Octal @ 80 МГц** |
| Framework | ESP-IDF **v5.5.1** + **ESP-ADF** (аудіо-пайплайн) |
| Bluetooth | BLE 5.0 (2M PHY), без Classic |
| USB | USB Host (MSC) — флешка з музикою |

## Можливості

- **MP3/FLAC/WAV/AAC/OGG/OPUS/AMR** з USB-флешки (ESP-ADF пайплайн)
- Плейлист, збереження позиції відтворення, автоперехід треків
- **BLE**: керування, статус, плейлист, логи, OTA
- **Web UI** (Wi-Fi AP/STA): плеєр, налаштування, логи, OTA
- **OTA** трьома шляхами: Wi-Fi (HTTP), BLE (бінарні кадри), UART → Slave
- **CAN** (TWAI) + Wi-Fi CAN-sniffer + емуляція **ELM327**
- Режими сну (idle / light / deep), NVS-налаштування
- Логи за модулями (5 груп, вкл/викл на ходу)
- Трансляція аудіо зі Slave по I2S (external mode)

## Архітектура

```
        ┌──────────────┐   BLE (GATT, JSON-RPC)   ┌─────────────────┐
        │  Android app │ ───────────────────────► │                 │
        └──────────────┘                          │      MASTER     │
        ┌──────────────┐   Wi-Fi (HTTP/web UI)    │    ESP32-S3     │
        │   Браузер    │ ───────────────────────► │                 │
        └──────────────┘                          └───┬─────────┬───┘
                                                      │ UART1   │ I2S (RX)
                                             115200   │         │ BCLK 41 / WS 39 / DIN 40
                                                      ▼         ▲
                                                ┌─────────────────┐
                                                │      SLAVE      │
                                                │   ESP32 A2DP    │ ◄── телефон (Bluetooth)
                                                └─────────────────┘
```

## Розділи flash (16 МБ)

| Name | Type | Offset | Size | Призначення |
|---|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24 КБ | налаштування |
| otadata | data/ota | 0xF000 | 8 КБ | вибір OTA-слота |
| phy_init | data/phy | 0x11000 | 60 КБ | RF-калібрування |
| factory | app | 0x20000 | 3 МБ | заводська прошивка |
| ota_0 | app | 0x320000 | 3 МБ | слот A |
| ota_1 | app | 0x620000 | 3 МБ | слот B |
| storage | data/fat | 0x920000 | 5.1 МБ | FAT-розділ |
| **staging** | data/0x40 | 0xE00000 | 2 МБ | буфер OTA (образ перед застосуванням) |

> ⚠️ `staging` — **нестандартний** розділ. Після зміни таблиці розділів потрібне
> **повне прошивання по USB**. OTA таблицю розділів не оновлює.

## Збірка

Потрібні **ESP-IDF 5.5.1** та **ESP-ADF** (для аудіо-пайплайну).

```powershell
# 1. ESP-IDF
C:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1

# 2. ESP-ADF (один раз)
$env:ADF_PATH = "C:\Espressif\esp-adf"      # шлях до ADF
# ADF має бути сумісний з IDF 5.5.1 (гілка master або свій форк)

# 3. Збірка
cd D:\emulators\Ford-DensO-mp3-master
idf.py set-target esp32s3
idf.py build
```

Результат: `build/Ford-DensO-mp3-master.bin`.

## Прошивка

**Перший раз / після зміни `partitions.csv` — тільки по USB:**

```powershell
idf.py -p COM6 flash monitor
```

**Далі — будь-яким OTA:**
- з Android-застосунку (BLE, Master або Slave);
- з веб-інтерфейсу (Wi-Fi, `/upload`);
- UART з іншого master (не використовується).

Швидкість OTA по BLE: **~20–50 КБ/с** (бінарні кадри, Write Without Response), образ 1.2 МБ ≈ 30–60 с.

## BLE

Сервіс: `11000000-0000-1000-8000-00805f9b34fb`

| Характеристика | UUID | Властивості |
|---|---|---|
| Command | `11010100-…` | write / write-no-response |
| Status | `11010200-…` | notify |
| Playlist | `11010300-…` | notify |
| Logs / RPC-відповіді | `11010400-…` | notify |

**Текстовий RPC:** `rpc:<команда>[:<json>]`, відповідь — JSON у notify.

| Команда | Опис |
|---|---|
| `version.get`, `system_info.get`, `can_statistics.get` | телеметрія |
| `wifi.get` / `wifi.set` | налаштування мережі |
| `sleep.get` / `sleep.set` | режим сну (`sleep_mode`, `idle_timeout_sec`) |
| `log_level.get/set`, `verbose.get/set` | рівень логів |
| `component_logs.get/set` | модулі логів (`player/connect/ota/car/system`) |
| `logs.get` | останні логи |
| `playlist.get` | плейлист частинами |
| `sniffer.set`, `device.restart` | сервісні |
| `ota.begin/data/end/abort/status` | OTA (текстовий, застарілий) |
| `slave_ota.*` | OTA Slave (застарілий) |
| **`file.begin/data/finish/status/abort`** | OTA-файл: `target: "master" \| "slave"` |

**Бінарні OTA-кадри** (швидкий шлях, `file.begin` → кадри → `file.finish`):

```
[0xA5][0x01][offset:4 LE][len:2 LE][payload]
```
Пишуться через **Write Without Response**, до 480 байт даних за кадр.

## Web UI

| Endpoint | Метод | Опис |
|---|---|---|
| `/` | GET | сторінка керування |
| `/player_cmd?cmd=…` | GET | play / pause / next / prev / switch_mode / set_pos |
| `/player_status`, `/playlist` | GET | стан і плейлист |
| `/upload` | POST | OTA Master (файл) |
| `/upload_slave` | POST | OTA Slave по UART |
| `/upload_to_storage` | POST | завантажити файл у FAT |
| `/ota_status`, `/version` | GET | прогрес і версії |
| `/wifi_settings`, `/sleep_settings`, `/log_level`, `/verbose_logs`, `/component_logs` | GET/POST | налаштування |
| `/logs` | GET | текстові логи |
| `/restart` | POST | перезавантаження |

## UART до Slave

| Параметр | Значення |
|---|---|
| Порт | UART1 |
| TX / RX (Master) | GPIO **8** / **9** |
| Baud | **115200** |
| Префікс команд | `0xAA` |
| Префікс OTA | `0x55` |

Команди (Master → Slave): `0x04` next, `0x05` prev, `0x06` версія, `0x07` play, `0x08` pause.

> UART **серіалізований**: під час OTA ніхто інший не пише в порт (м'ютекс + прапорець).
> Паралельні запити (опитування версії) раніше «з'їдали» ACK-и — це виправлено.

## OTA: три шляхи

1. **BLE → Master**: образ у PSRAM → `staging` → `esp_ota_*` → reboot.
2. **BLE → Slave**: образ у PSRAM → `staging` → UART-протокол (блоки по 240 Б з ACK).
3. **HTTP → Master/Slave**: те саме через веб-форму.

Прогрес і ETA рахуються в застосунку; застосунок також показує причину збою
(`invalid firmware`, `incomplete upload`, `base64`, `MTU`, …).

## Логи

5 модулів (перемикаються в застосунку та веб-UI):

| Модуль | Компоненти |
|---|---|
| 🎵 Плеєр | Player, Pipeline, Playlist |
| 📡 Зв'язок | BLE, WiFi, WiFi CAN Sniffer, Link |
| 💾 OTA | Web OTA, UART OTA |
| 🚗 Авто | CAN, ELM327 |
| ⚙️ Система | USB, Sleep, Console, NVS |

Стан зберігається в NVS (`log_groups`), при старті розгортається у прапорці компонентів.

## Сон

`/sleep_settings` (або `sleep.set`):
- `sleep_mode`: `0` — вимкнено, `1` — light sleep, `3` — deep sleep;
- `idle_timeout_sec`: таймаут простою в секундах (мінімум 10).

## Релізи

GitHub Actions (`.github/workflows/release.yml`) збирає прошивку і публікує реліз.

```bash
git add .
git commit -m "release: v1.1.0"
git push
git tag v1.1.0
git push origin v1.1.0
```

У реліз потрапляють:
- `Ford-DensO-mp3-master.bin` — образ для OTA (з телефона або веб-UI);
- `Ford-DensO-mp3-master-full.bin` — повний образ: `esptool write_flash 0x0`;
- `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` — для ручного прошивання.

Збірка йде в контейнері `espressif/idf:v5.5.1` з ESP-ADF, закріпленим на комміті
`fcce23e3` (той самий, що локально). Час — 10–20 хв (ADF + компоненти).

Без тега можна запустити вручну: **Actions → Release firmware → Run workflow** —
тоді артефакти просто зберігаються в run-і, без релізу.


## Типові проблеми

| Симптом | Причина / рішення |
|---|---|
| `No 'staging' partition` | прошита стара таблиця розділів → повне прошивання по USB |
| OTA падає на ~50 КБ | несумісні версії прошивки/застосунку, або BLE-лінк; див. `monitor` |
| `Invalid JSON` в логах плати | запис обрізано (MTU) — онови застосунок/прошивку |
| Slave не перемикає треки | UART 115200 на обох; див. `UART cmd 0x04 sent` у логах |
| Немає звуку з телефона | Master має бути в external mode; перевір I2S (41/39/40) |
| Плата не прокидається | deep sleep вимикає USB — перепідключи живлення |

## Ліцензія

Код проєкту — для особистого використання. ESP-IDF та ESP-ADF — під своїми ліцензіями Espressif.
