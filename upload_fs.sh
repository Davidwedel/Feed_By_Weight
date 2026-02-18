#!/bin/bash
set -e

HOST="${1:-192.168.1.205}"
BACKUP_FILE="history_backup.csv"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

cd "$SCRIPT_DIR"

echo "=== Step 1: Backup feed history ==="
python3 backup_history.py backup --host "$HOST" --file "$BACKUP_FILE"

echo ""
echo "=== Step 2: Upload filesystem ==="
pio run --target uploadfs

echo ""
echo "=== Step 3: Waiting for ESP32 to reboot ==="
sleep 15

echo ""
echo "=== Step 4: Restore feed history ==="
python3 backup_history.py restore --host "$HOST" --file "$BACKUP_FILE"

echo ""
echo "=== Done ==="
