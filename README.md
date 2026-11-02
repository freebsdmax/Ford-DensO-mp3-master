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

- **MP3/FLAC/WAV/AAC/OGG/OPUS/AMR** з USB-флешки (декодер вибирається за розширенням)
- Плейлист з **ID3-назвами** (`Виконавець — Назва`), автозбереження позиції, автоперехід
- **Програмна гучність 0–100 %** (PCM5102 не має апаратного регулятора) — для USB і Bluetooth
- **Метадані з телефона (AVRCP)**: назва, виконавець, позиція, play/pause — у BLE і Web
- **BLE**: керування, статус, плейлист, логи, OTA, усі налаштування
- **Web UI** (Wi-Fi AP/STA): плеєр, налаштування, логи, OTA, токен доступу
- **OTA**: Wi-Fi (HTTP), BLE (бінарні кадри → `/storage` → прошивка), UART → Slave
- **CAN** (TWAI, 125/250/500 кбіт/с, налаштовувані ID і маски) + Wi-Fi CAN-sniffer + **ELM327**
- Режими сну (idle / light / deep), NVS-налаштування
- Логи за модулями (5 груп) + **«чорна скринька»** у `/storage` (переживає перезавантаження)
- Трансляція аудіо зі Slave по I2S (external mode) + **профілі буферів** (затримка/стійкість)
- Статична IP/DNS для STA, токен доступу до веб/OTA, змінюване ім'я BLE
- **Продовження з місця** після паузи, перезавантаження чи повторної вставки флешки
  (позиція в NVS кожні 20 с; seek виконується **до** відкриття файлу)
- **240 МГц**, без періодичних записів у flash під час відтворення (немає «тиків»)
- Один автоматичний перезапуск, якщо флешка не перелічилась після ресету
  (лише коли флешкою вже користувались)

## Налаштування в NVS (`audio_player`)

| Ключ | Що | Типово |
|---|---|---|
| `volume` | гучність, % | 80 |
| `seek_step` / `seek_mode` | крок перемотки (5–30 с) / метод (0=натискання, 1=утримання) | 10 / 0 |
| `can_bitrate`, `can_id`, `can_mn`, `can_mp`, `can_mm`, `can_deb` | CAN-шина | 125, 0x2D5, 0x40, 0x80, 0x10, 500 |
| `audio_profile` | 0=низька затримка, 1=збалансований, 2=стійкий | 1 |
| `log_buf_kb` / `log_persist` | розмір буфера логів / «чорна скринька» | 4 / 1 |
| `ble_name` | ім'я пристрою по BLE | `ESP32_MP3_Player` |
| `auth_token` | токен HTTP/OTA (порожній = вимкнено) | — |
| `net_static`, `net_ip`, `net_gw`, `net_mask`, `net_dns1`, `net_dns2` | статична IP для STA | DHCP |
| `track_index`, `position_ms`, `save_time_ms` | останній трек і позиція (продовження з місця) | — |
| `last_source` | останній **вручну обраний** режим: 0 = USB, 1 = External | 0 |

> Частина налаштувань застосовується після перезавантаження (ім'я BLE, розмір буфера логів,
> статична IP, профіль буферів). Решта — одразу.

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

Для локальної збірки повного образу (щоб потім залити `write_flash 0x0`):

```powershell
esptool --chip esp32s3 merge_bin -o build/Ford-DensO-mp3-master-full.bin `
  --flash_mode dio --flash_freq 80m --flash_size 16MB `
  0x0 build/bootloader/bootloader.bin `
  0x8000 build/partition_table/partition-table.bin `
  0xf000 build/ota_data_initial.bin `
  0x20000 build/Ford-DensO-mp3-master.bin
```

> `merge_bin` заповнює проміжки `0xFF`, тому повний образ **затирає NVS**
> (налаштування/Wi-Fi скидаються). OTA через застосунок або веб NVS не чіпає.

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
| `logs.get` | останні логи з кільцевого буфера |
| `logs.cfg` / `logs.set` | розмір буфера (2/4/8/16 КБ) і «чорна скринька» |
| `playlist.get` | плейлист частинами (назви з ID3) |
| `media.get` | що грає на телефоні (AVRCP): назва, виконавець, позиція |
| `volume.get` / `volume.set` | програмна гучність 0–100 % |
| `seek.get` / `seek.set` | крок перемотки (5–30 с) і метод (натискання/утримання) |
| `can.get` / `can.set` | швидкість CAN, ID кнопок, маски, антибрязкіт |
| `audio.get` / `audio.set` | профіль аудіо-буферів (0/1/2) |
| `net.get` / `net.set` | статична IP/DNS для STA |
| `ble.get` / `ble.set` | ім'я пристрою по BLE (після перезавантаження) |
| `auth.get` / `auth.set` | токен доступу до HTTP/OTA (порожній = вимкнено) |
| `sniffer.set`, `device.restart` | сервісні |
| `ota.begin/data/end/abort/status` | OTA (текстовий, застарілий) |
| `slave_ota.*` | OTA Slave (застарілий) |
| **`file.begin/data/finish/status/abort`** | OTA-файл: `target: "master" \| "slave" \| "storage"` |
| **`file.flash`** | прошити образ із `/storage` (`name`, `target`) |
| **`file.stored`** | розміри збережених образів |

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
| `/volume?value=0..100` | GET | гучність |
| `/seek_settings?step_s=&mode=` | GET/POST | перемотка |
| `/can_settings?bitrate=&id=&mn=&mp=&mm=&debounce=` | GET/POST | CAN-шина |
| `/audio_profile?profile=0/1/2` | GET/POST | профіль аудіо-буферів |
| `/logs_config?buf_kb=&persist=` | GET/POST | буфер логів і «чорна скринька» |
| `/net_settings?static=&ip=&gw=&mask=&dns1=&dns2=` | GET/POST | статична IP |
| `/ble_settings?name=` | GET/POST | ім'я BLE |
| `/logfile`, `/logfile?old=1` | GET | завантажити `/storage/log.txt` або `log.old` |
| `/logs` | GET | текстові логи (RAM) |
| `/restart` | POST | перезавантаження |

> Усі змінювальні ендпоінти захищені токеном (`?token=…` або `X-Auth-Token`), якщо він заданий
> (налаштування `auth.set`).

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

Пробудження — по CAN RX (ext0). Після пробудження плата **перезавантажується**
(deep sleep = повний старт), тому флешка може не перелічитись, якщо не зникало
5 В: у v1.1.0 передбачений один авто-ребут у перші 15 с (див. нижче). Позиція
відтворення зберігається в NVS і після сну відновлюється.

## Що нового у v1.1.0

- **Звук з USB**: кастомний елемент гучності тепер викликає
  `audio_element_output()` — раніше він «з'їдав» увесь потік PCM (звуку не було,
  гучність і перемотка нічого не змінювали).
- **Продовження з місця**: у цій версії ADF `audio_element_set_byte_pos()` лише
  записує лічильник, а `fatfs` переміщує файл **при відкритті**. Тому позиція
  виставляється до `audio_pipeline_run`, а байти рахуються за реальним бітрейтом.
  Раніше повзунок показував 20 с, а звук ішов з початку.
- **Час пісні**: тривалість рахується як `байти × 8 / бітрейт` (раніше — за
  хардкодом 128 кбіт/с, через що 320 kbps файл «біг» у 2.5 раза швидше).
- **Флешка після ресету**: контролер іноді не перелічує її без зняття 5 В, тому
  прошивка робить рівно один авто-ребут у перші 15 с (і лише якщо флешкою вже
  користувались). Під час відтворення ребутів немає.
- **Сон**: прибрано тестовий таймер пробудження (20 с), який перезавантажував
  плату після засинання.
- **Джерело звуку**: у NVS пишеться лише **ручний** вибір USB/External;
  автоматичні перемикання не запам'ятовуються і не перебивають вибір.
- **Стабільність USB**: прибрано `usb_host_uninstall()` (усередині
  `hub_uninstall()` робить `abort()`), немає ребут-циклу.
- **Менше записів у flash**: позиція — раз на 20 с, «чорна скринька» — раз на 5 хв;
  лог `AUDIO_EVT: no space in external queue` більше не спамить.
- **240 МГц** замість 160.

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
| Флешка не визначається після ресету | контролер не перелічився без зняття 5 В; v1.1.0 робить один авто-ребут, інакше — перевставити флешку |
| Після вставки флешки грає з початку | онови до v1.1.0+; у лозі має бути `Відновлення позиції до старту` і `file position:` ≠ 0 |
| Час пісні біжить удвічі швидше | стара прошивка вважала бітрейт 128 кбіт/с — онови до v1.1.0 |
| Немає звуку з USB | перевір `AUDIO_PIPELINE: Pipeline started`; v1.1.0 виправляє елемент гучності |
| Плата сама перезавантажується після сну | у тестових збірках був таймер пробудження; у v1.1.0 його немає |

## Ліцензія

Код проєкту — для особистого використання. ESP-IDF та ESP-ADF — під своїми ліцензіями Espressif.
