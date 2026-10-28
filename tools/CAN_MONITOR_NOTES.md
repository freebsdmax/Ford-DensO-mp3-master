# CAN-моніторинг — робоча памʼятка

> **Тригер:** коли користувач каже «підключили до CAN» / «плата на шині» —
> одразу запускати `tools\can_monitor.py` і показувати кадри в реальному часі.
> Нічого не перепитувати про мережу/адресу — все вже налаштовано нижче.

## Стан (перевірено)

| Що | Значення |
|---|---|
| Master IP | **192.168.88.244** (STA, домашня мережа 192.168.88.0/24) |
| Ноут | 192.168.88.103 (Wi-Fi, інтернет є) |
| UDP-приймач | `0.0.0.0:43605` (0xAA55) |
| Пристрій шле на | **192.168.88.255:43605** (STA broadcast) |
| Снайфер | увімкнений, стартує сам після boot |
| Прошивка | build `23:00:02` — містить фікс адреси призначення |

Підтвердження з логів плати (`/logs`):

```
WiFiCanSniffer: UDP destination: STA broadcast 192.168.88.255:43605
WiFiCanSniffer: UDP socket bound successfully
WiFiCanSniffer: Wi-Fi CAN-Sniffer READY
```

## Команда

```powershell
cd D:\emulators\Ford-DensO-mp3-master
python tools\can_monitor.py                 # всі кадри
python tools\can_monitor.py --id 0x7E0      # фільтр по ID
python tools\can_monitor.py --csv can.csv   # у файл
python tools\can_monitor.py --duration 30   # автостоп через 30 с
```

Або через хелпер (сам знайде IP плати і перевірить снайфер):

```powershell
powershell -ExecutionPolicy Bypass -File tools\can_watch.ps1
```

## Формат даних

```
[signature 0xAA55AA55 : 4]
  для кожного кадру: [id:4 LE][interval_ms:2 LE][length:1][data:8]
```

## Залізо (коли підключатимуть)

- трансивер TJA1050 / SN65HVD230 / MCP2551;
- `TX` → **GPIO 4**, `RX` → **GPIO 15**, спільна **GND**, живлення трансивера;
- термінатори **120 Ом** на кінцях шини;
- швидкість **500 кбіт/с** (якщо в авто 250 — змінити `change_can_bitrate(250)`).

## Що робити, якщо кадрів немає

1. `curl http://192.168.88.244/player_status` → `sniffer_enabled` має бути `true`.
2. `curl http://192.168.88.244/logs` → шукати `UDP destination` і `CAN`.
3. Перевірити термінатори/живлення трансивера і що CANH/CANL не переплутані.
4. Якщо плата переїхала в AP-режим — адреса стане `192.168.4.255`, тоді ноут
   має бути в її мережі (або залити фікс знову — він уже в прошивці).

## Виправлено раніше

`refresh_destination()` у `components/wifi_can_sniffer/wifi_can_sniffer.c`:
адреса призначення тепер обчислюється з активної мережі (STA → broadcast підмережі,
інакше AP 192.168.4.255, інакше `pc_ip_str`). До цього було жорстко `192.168.4.255`,
тому в STA-режимі пакети не доходили до ПК.
