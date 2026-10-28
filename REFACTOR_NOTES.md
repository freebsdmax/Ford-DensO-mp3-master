# REFACTOR NOTES / СТАН ПРОЄКТУ

Оновлено: фінальний стан після рефакторингу, USB/BLE OTA, логів і UTF-8.

## 1. Очищення (виконано)
- Видалено одноразові скрипти `add_*.py`, `check_*.py`, `fix_*.py`, `show_*.py`, `verify_*.py`.
- Видалено `main/web_ota.c.bak`, артефакти `line505.txt`, `monitoring_ctx.txt`, `switch_ctx.txt`.
- Видалено `sdkconfig.old`, `components/elm327_wifi/elm327_wifi_old.c`.
- `.gitignore` доповнено build-артефактами, `tmp_scripts/`, `managed_components/`.

## 2. Структура модулів (main/)
| Файл | Призначення |
|---|---|
| `player_master.c` | `app_main`, головний цикл повідомлень (`app_queue`), HTTP-хелпери плейлиста/статусу |
| `player_nvs.c` | NVS-хелпери, credentials, playback position, init/clear |
| `player_playlist.c` | плейлист, сканування USB, UTF-8 конвертер назв |
| `player_pipeline.c` | `player_task`, I2S, audio pipeline, volume, seek, зовнішній вхід |
| `player_console.c` | `terminal_commands_task`, `app_queue`, статистика задач/стеку |
| `player_sleep.c` | налаштування сну, `enter_deep_sleep`, `enter_configured_sleep` |
| `player_usb.c` | USB MSC: boot scan, re-init, mount/unmount, задачі |
| `player_can.c` | CAN/TWAI, `can_task`, `process_can_message`, Wi-Fi sniffer callbacks |
| `player_ble.c` | BLE RPC, OTA RPC, логи, статус/плейлист |
| `player_ble_ota.c` | BLE OTA: буфер у PSRAM → запис у OTA-розділ |
| `player_wifi.c` | Wi-Fi налаштування, STA підключення |
| `player_link.c` | UART-лінк (master↔slave), GPIO ISR |
| `player_logs.c` | прапорці логів по модулях + apply/save/load |
| `player_version.h` | версія + `player_git_version.h` (генерується CMake) |
| `app_messages.h` | `app_message_t`, `app_queue` |
| `web_ota.c` | веб-UI, OTA (Wi-Fi/UART), налаштування, моніторинг |
| `uart_ota_master.c` | UART OTA для slave |

## 3. Версіонування
- `main/CMakeLists.txt` генерує `player_git_version.h` з `git rev-parse --short HEAD`.
- `player_version.h` включає `PLAYER_GIT_HASH`.

## 4. Оптимізація
- `CONFIG_COMPILER_OPTIMIZATION_PERF` (`-O2`).
- `CONFIG_FREERTOS_USE_TRACE_FACILITY=y`; консоль: `t` — стеки задач, `h` — heap.
- PSRAM: `CONFIG_SPIRAM=y`, `MODE_OCT`, `SPEED_80M`, тип `ESPPSRAM64` (N16R8).
- Орієнтовний розмір образу: ~1.66 MB (`-O2`).

## 5. FATFS / кодування назв
- `CONFIG_FATFS_LFN_HEAP=y`, `CONFIG_FATFS_MAX_LFN=255`.
- `CONFIG_FATFS_CODEPAGE_866=y`.
- `CONFIG_FATFS_API_ENCODING_UTF_8=y` — FatFS віддає UTF-8 (повний Unicode).
- `cp866_to_utf8()` — сумісний декодер: якщо вже UTF-8 — без змін; інакше авто CP866/CP1251.
- `playlist_get_display_name()` — ім'я без шляху в UTF-8.
- HTTP `/playlist` і BLE `playlist.get` віддають UTF-8; BLE — посторінково (offset/count).

## 6. BLE OTA
- `ota.begin` `{size}` → буфер у PSRAM.
- `ota.data` `{offset, data(base64)}` → копіювання в буфер, контроль пропусків.
- `ota.end` → `esp_ota_begin/write(4KB)/end/set_boot_partition` → restart.
- `ota.status`, `ota.abort`.
- Android: `bleOtaUpdate()` з підтвердженням кожного чанка, повтором з `written`, MTU-aware чанк, `ensureMtu()`.
- BLE сервіс: `WRITE_NR`, 2M PHY, черга RPC 32.

## 7. USB
- Boot scan у циклі `usb_host_lib_handle_events` (не блокує енумерацію).
- До 3 re-init USB-стека, якщо `devices=0` (bus reset).
- `usb_forget_devices()` перед re-init (VFS/MSC очищення).
- `allocate_new_msc_device(..., is_new)` + ігнорування дублів connect.
- Сканування зупиняється після виявлення USB або в external-режимі; лог лише при зміні кількості.
- Консоль `u` — ручний re-init (тільки якщо не змонтовано).

## 8. Веб-UI
- Додано `</script>` (раніше ламало всі вкладки).
- Вкладки: Player / OTA / Settings / Monitoring.
- Плейлист: оновлення при відкритті вкладки + кожні 5 с, поки порожній.
- Логи по модулях: Player, WiFi CAN Sniffer, Web OTA, UART OTA, ELM327 + Pipeline, CAN, USB, BLE, WiFi, Sleep, Playlist, Console, Link, NVS.
- `/component_logs` приймає JSON (Android) і plain text (web).
- Кнопку `C (Disable Sleep)` прибрано з Monitoring.

## 9. Консоль (UART)
- `n` next, `p` prev, `m` mode, `c` flush I2S, `s` disable sleep, `w` WiFi STA, `t` task/stack stats, `h` heap, `u` USB re-init.

## 10. Android
- BLE/HTTP транспорт, вибір треку (клік по всьому рядку), назви у 2 рядки.
- BLE OTA: `Update over BLE`, прогрес.
- UTF-8 назви.
- APK: `AndroidPlayer/app/build/outputs/apk/debug/app-debug.apk`.
- `local.properties`: `sdk.dir=C:\Users\Max\AppData\Local\Android\Sdk`.

## 11. Відомі обмеження
- CP866 не містить `І і`, `Ґ ґ`; але з `FATFS_API_ENCODING_UTF_8` довгі імена читаються без втрат. Для 8.3 short names втрати можливі.
- Якщо плата не керує VBUS програмно, після reset може знадобитися фізичне перевтикання (re-init допомагає не завжди).
- Warning `-Woverride-init` у `player_can.c` (безпечний).

## 12. Правила роботи (для агента)
1. Змінювати код по одному блоку, перевіряти збірку після кожної зміни.
2. При помилці — виправляти, не відкочувати файл.
3. Тільки PowerShell-команди (`Get-Content`, `Select-String`, `Set-Content`); без `cat/tail/head/sed/grep/heredoc`.
4. Тимчасові скрипти — в `tmp_scripts/` (в .gitignore), видаляти після використання.
5. Не створювати тимчасові файли в корені проєкту.
6. Після успішної збірки — автоматично продовжувати без зайвих питань.
