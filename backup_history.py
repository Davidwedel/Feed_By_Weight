#!/usr/bin/env python3
"""Backup and restore feed history from ESP32 LittleFS.

Usage:
    python backup_history.py backup  [--host IP]  [--file PATH]
    python backup_history.py restore [--host IP]  [--file PATH]
"""

import argparse
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


def main():
    parser = argparse.ArgumentParser(description="Backup/restore ESP32 feed history")
    parser.add_argument("action", choices=["backup", "restore"])
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"ESP32 IP (default: {DEFAULT_HOST})")
    parser.add_argument("--file", default=DEFAULT_FILE, help=f"CSV file path (default: {DEFAULT_FILE})")
    args = parser.parse_args()

    if args.action == "backup":
        sys.exit(backup(args.host, args.file))
    else:
        sys.exit(restore(args.host, args.file))


if __name__ == "__main__":
    main()
