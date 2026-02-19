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
echo "=== Step 3: Waiting for ESP32 to come online ==="
SERIAL_PORT=$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -1)
TIMEOUT=60

if [ -n "$SERIAL_PORT" ]; then
    echo "Monitoring $SERIAL_PORT (timeout ${TIMEOUT}s)..."
    echo "-------------------------------------------"
    stty -F "$SERIAL_PORT" 115200 raw -echo
    FOUND=false
    while IFS= read -r -t "$TIMEOUT" line; do
        echo "$line"
        if [[ "$line" == *"System initialization complete"* ]]; then
            FOUND=true
            break
        fi
    done < "$SERIAL_PORT"
    echo "-------------------------------------------"
    if $FOUND; then
        echo "ESP32 is online!"
        sleep 2  # Let network interface come up
    else
        echo "Timed out waiting for startup. Trying restore anyway..."
    fi
else
    echo "No serial port found, falling back to sleep..."
    sleep 15
fi

echo ""
echo "=== Step 4: Restore feed history ==="
python3 backup_history.py restore --host "$HOST" --file "$BACKUP_FILE"

echo ""
echo "=== Done ==="
