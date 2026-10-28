#!/usr/bin/env python3
"""
Ford Player / ESP32 — приймач CAN-кадрів від Wi-Fi CAN-Sniffer.

Пристрій (Master ESP32-S3) піднімає точку доступу:
    SSID:     ESP32-CAN-Sniffer
    Password: 12345678
    UDP:      0xAA55 (43605), broadcast 192.168.4.255

Формат датаграми (усі поля little-endian):
    [signature:4 = 0xAA55AA55]
    для кожного кадру:
        [id:4] [interval:2] [length:1] [data:8]

Використання:
    python can_monitor.py                  # слухати 0.0.0.0:43605
    python can_monitor.py --id 0x7E0      # тільки один ID
    python can_monitor.py --csv can.csv   # писати у CSV
    python can_monitor.py --quiet         # без кадрів, лише статистика

Перед запуском: підключи ПК до Wi-Fi "ESP32-CAN-Sniffer" і увімкни
снайфер (веб-UI → toggle, або BLE `sniffer.set {enable:1}`).
"""

import argparse
import csv
import socket
import struct
import sys
import time
from collections import defaultdict

SIGNATURE = 0xAA55AA55
FRAME = struct.Struct("<IHB8s")   # id, interval(ms), length, data
DEFAULT_PORT = 0xAA55             # 43605


def describe_id(can_id: int) -> str:
    if can_id & 0x80000000:
        # 29-bit extended id (біт 31 може використовуватись як прапорець EFF)
        return f"0x{can_id & 0x1FFFFFFF:08X}"
    if can_id > 0x7FF:
        return f"0x{can_id:08X}"
    return f"0x{can_id:03X}"


def main() -> int:
    # Windows consoles default to cp1251/cp866 and crash on "Δ"
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    ap = argparse.ArgumentParser(description="ESP32 Wi-Fi CAN sniffer listener")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help="UDP port (default 43605)")
    ap.add_argument("--bind", default="0.0.0.0", help="listen address")
    ap.add_argument("--id", default=None, help="filter by CAN id, e.g. 0x7E0")
    ap.add_argument("--csv", default=None, help="write frames to CSV")
    ap.add_argument("--quiet", action="store_true", help="no per-frame output")
    ap.add_argument("--duration", type=float, default=0,
                    help="stop after N seconds (0 = forever)")
    args = ap.parse_args()

    want_id = int(args.id, 0) if args.id else None

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind((args.bind, args.port))
    sock.settimeout(1.0)

    csv_file = None
    csv_writer = None
    if args.csv:
        csv_file = open(args.csv, "w", newline="", encoding="utf-8")
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(["timestamp", "id", "dlc", "data", "interval_ms"])

    counters = defaultdict(int)
    last_ts = defaultdict(float)
    total = 0
    t0 = time.time()
    deadline = (t0 + args.duration) if args.duration else 0.0

    print(f"Listening UDP {args.bind}:{args.port} ... (Ctrl+C to stop)")
    if want_id is not None:
        print(f"Filter: {describe_id(want_id)}")

    try:
        while True:
            try:
                data, addr = sock.recvfrom(4096)
            except socket.timeout:
                elapsed = time.time() - t0
                if deadline and time.time() >= deadline:
                    break
                rate = total / elapsed if elapsed > 0 else 0
                print(f"\r[{elapsed:6.1f}s] frames={total} ({rate:5.1f}/s) ids={len(counters)}   ",
                      end="", flush=True)
                continue

            if len(data) < 4 + FRAME.size:
                continue

            sig = struct.unpack_from("<I", data, 0)[0]
            if sig != SIGNATURE:
                print(f"\n! unknown datagram from {addr[0]}: {len(data)} bytes, sig=0x{sig:08X}")
                continue

            if deadline and time.time() >= deadline:
                break

            offset = 4
            while offset + FRAME.size <= len(data):
                can_id, interval, dlc, raw = FRAME.unpack_from(data, offset)
                offset += FRAME.size
                total += 1

                if want_id is not None and can_id != want_id:
                    continue

                counters[can_id] += 1
                now = time.time()
                dt = (now - last_ts[can_id]) * 1000.0 if can_id in last_ts else 0.0
                last_ts[can_id] = now
                payload = raw[:min(dlc, 8)]

                if not args.quiet:
                    hexdata = " ".join(f"{b:02X}" for b in payload)
                    print(f"\n{now:12.3f}  {describe_id(can_id):>12}  "
                          f"dlc={dlc}  {hexdata:<23}  dt={dt:6.1f}ms  (dev {interval}ms)",
                          flush=True)

                if csv_writer:
                    csv_writer.writerow([f"{now:.3f}", describe_id(can_id), dlc,
                                         " ".join(f"{b:02X}" for b in payload), interval])

    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
        if csv_file:
            csv_file.close()

    print("\n\n--- summary ---")
    elapsed = time.time() - t0
    print(f"duration: {elapsed:.1f}s, frames: {total}, ids: {len(counters)}")
    for can_id, n in sorted(counters.items(), key=lambda kv: -kv[1]):
        print(f"  {describe_id(can_id):>12}: {n:7d}  ({n / elapsed:6.1f}/s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
