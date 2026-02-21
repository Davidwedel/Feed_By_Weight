#!/usr/bin/env python3
"""Backup, restore, and display feed history from ESP32 LittleFS.

Usage:
    python backup_history.py backup  [--host IP]  [--file PATH]
    python backup_history.py restore [--host IP]  [--file PATH]
    python backup_history.py show    [--file PATH]
"""

import argparse
from datetime import datetime
import sys
import urllib.request

DEFAULT_HOST = "192.168.1.205"
DEFAULT_FILE = "history_backup.csv"


def backup(host, filepath):
    url = f"http://{host}/api/history/backup"
    print(f"Downloading history from {url} ...")
    try:
        with urllib.request.urlopen(url, timeout=10) as resp:
            data = resp.read()
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    with open(filepath, "wb") as f:
        f.write(data)

    lines = data.count(b"\n")
    print(f"Saved {lines} entries ({len(data)} bytes) to {filepath}")
    return 0


def restore(host, filepath):
    try:
        with open(filepath, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        print(f"Error: {filepath} not found", file=sys.stderr)
        return 1

    if len(data) == 0:
        print("Backup file is empty, nothing to restore.")
        return 0

    url = f"http://{host}/api/history/restore"
    print(f"Uploading {len(data)} bytes to {url} ...")
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Content-Type", "text/csv")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read().decode()
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    lines = data.count(b"\n")
    print(f"Restored {lines} entries. Response: {body}")
    return 0


def show(filepath):
    try:
        with open(filepath, "r") as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"Error: {filepath} not found", file=sys.stderr)
        return 1

    entries = []
    for line in lines:
        line = line.strip()
        if not line:
            continue
        parts = line.split(",", 6)
        if len(parts) < 6:
            continue
        ts = int(parts[0])
        cycle = int(parts[1]) + 1
        target = float(parts[2])
        actual = float(parts[3])
        duration = int(parts[4])
        alarm = int(parts[5])
        reason = parts[6] if len(parts) > 6 else ""
        entries.append((ts, cycle, target, actual, duration, alarm, reason))

    if not entries:
        print("No history entries found.")
        return 0

    # Header
    print(f"{'Date/Time':<22} {'Cycle':>5} {'Target':>8} {'Actual':>8} {'Duration':>8} {'Status'}")
    print("-" * 75)

    for ts, cycle, target, actual, duration, alarm, reason in entries:
        dt = datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S")
        dur = f"{duration // 60}:{duration % 60:02d}"
        status = reason if reason else "OK"
        print(f"{dt:<22} {cycle:>5} {target:>8.2f} {actual:>8.2f} {dur:>8} {status}")

    print(f"\n{len(entries)} entries")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Backup/restore/show ESP32 feed history")
    parser.add_argument("action", choices=["backup", "restore", "show"])
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"ESP32 IP (default: {DEFAULT_HOST})")
    parser.add_argument("--file", default=DEFAULT_FILE, help=f"CSV file path (default: {DEFAULT_FILE})")
    args = parser.parse_args()

    if args.action == "backup":
        sys.exit(backup(args.host, args.file))
    elif args.action == "restore":
        sys.exit(restore(args.host, args.file))
    else:
        sys.exit(show(args.file))


if __name__ == "__main__":
    main()
