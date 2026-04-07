#!/usr/bin/env python3
"""
Modbus TCP test script for BinTrac HouseLink HL-10E.
Connects to the BinTrac over TCP and reads bin weight values using the
Modbus protocol (function code 4 - Read Input Registers).

Usage:
  python test_modbus.py          # Continuous connection monitor (prints OK every 5s)

Requirements: pip install pymodbus
"""

import argparse
import csv
import glob
import os
import sys
import time
from datetime import datetime, date, timedelta

from pymodbus.client import ModbusTcpClient

_log_file = None
CSV_DIR = "weight_logs"

# Mutable container so the weight loop can swap files on day rollover
_csv = {"writer": None, "file": None}


def ts():
    """Return current timestamp string for log output."""
    return datetime.now().strftime("%H:%M:%S")


def open_csv_for_today():
    """Open (or append to) today's CSV file, updating the _csv container."""
    os.makedirs(CSV_DIR, exist_ok=True)
    filename = os.path.join(CSV_DIR, f"weights_{date.today().strftime('%Y-%m-%d')}.csv")
    is_new = not os.path.exists(filename)
    f = open(filename, "a", newline="")
    writer = csv.writer(f)
    if is_new:
        writer.writerow(["timestamp", "bin_a", "bin_b", "bin_c", "bin_d", "total"])
    _csv["writer"] = writer
    _csv["file"] = f
    print(f"CSV logging to {filename}")


def purge_old_csvs():
    """Delete CSV files older than 3 days."""
    cutoff = date.today() - timedelta(days=3)
    for path in glob.glob(os.path.join(CSV_DIR, "weights_*.csv")):
        basename = os.path.basename(path)
        try:
            file_date = datetime.strptime(basename, "weights_%Y-%m-%d.csv").date()
            if file_date < cutoff:
                os.remove(path)
                print(f"Deleted old log: {path}")
        except ValueError:
            pass


def log(msg=""):
    """Print to terminal and optionally write to log file."""
    print(msg)
    if _log_file:
        _log_file.write(msg + "\n")
        _log_file.flush()

# Connection settings for the BinTrac HouseLink HL-10E
HOST = "192.168.1.173"  # BinTrac IP address on the local network
PORT = 502              # Standard Modbus TCP port
DEVICE_ID = 1           # Modbus unit/station ID (set on the HL-10E Devices page)

# Register addresses for each bin. Each bin uses 2 registers (32-bit value),
# so they're spaced 2 apart. The HL-10E docs show these as 1000-based, but
# the Modbus protocol is 0-indexed so we subtract 1 before sending.
BIN_A_ADDR = 1000
BIN_B_ADDR = 1002
BIN_C_ADDR = 1004
BIN_D_ADDR = 1006
ALL_BINS_ADDR = 1000  # Start here and read 8 registers to get all 4 bins


def parse_bin_weight(registers, offset=0):
    """Convert a pair of Modbus registers into a bin weight in lbs.

    The BinTrac stores each bin's weight as a 32-bit signed integer split
    across two consecutive 16-bit registers (big-endian). This function
    recombines them and handles the special "disabled" marker value.

    Args:
        registers: List of 16-bit register values from the Modbus read.
        offset: Index into the list where this bin's first register starts.
                Bin A=0, B=2, C=4, D=6.

    Returns:
        Weight in lbs as a float, 0.0 if the bin is disabled, or None if
        the offset is out of range.
    """
    if offset + 1 >= len(registers):
        return None

    # Each bin spans 2 registers: [high 16 bits, low 16 bits]
    high_word = registers[offset]
    low_word = registers[offset + 1]

    # Reassemble into a single 32-bit value
    raw_value = (high_word << 16) | low_word

    # Python ints are unsigned by default, so manually handle the sign bit
    if raw_value > 2147483647:
        raw_value = raw_value - 4294967296

    # BinTrac uses -32767 (0xFFFF8001) to mean "bin not connected/disabled"
    if raw_value == -32767 or raw_value == 0xFFFF8001:
        return 0.0

    return float(raw_value)


def read_registers(client, start_addr, num_registers):
    """Send a Modbus "Read Input Registers" request and return the results.

    Handles the address offset (BinTrac docs use 1-based addresses, but the
    Modbus wire protocol is 0-based) and prints what it's doing for debugging.

    Args:
        client: Connected ModbusTcpClient instance.
        start_addr: 1-based register address (e.g. 1000 for Bin A).
        num_registers: How many 16-bit registers to read (2 per bin).

    Returns:
        List of 16-bit register values, or None on error.
    """
    # Subtract 1 because Modbus protocol addresses are 0-indexed
    protocol_addr = start_addr - 1

    log(f"{ts()} Reading {num_registers} registers from address {start_addr} (protocol addr: {protocol_addr})")

    result = client.read_input_registers(protocol_addr, count=num_registers, device_id=DEVICE_ID)

    if result.isError():
        log(f"{ts()} ERROR: Modbus error - {result}")
        return None

    log(f"{ts()} Read {len(result.registers)} registers: {result.registers}")
    return result.registers


def print_weights(registers):
    """Parse all 4 bins from an 8-register read and print their weights."""
    bin_a = parse_bin_weight(registers, 0)
    bin_b = parse_bin_weight(registers, 2)
    bin_c = parse_bin_weight(registers, 4)
    bin_d = parse_bin_weight(registers, 6)
    log(f"  Bin A: {bin_a} lbs")
    log(f"  Bin B: {bin_b} lbs")
    log(f"  Bin C: {bin_c} lbs")
    log(f"  Bin D: {bin_d} lbs")
    log(f"  Total: {bin_a + bin_b + bin_c + bin_d} lbs")


def main():
    """Entry point. Connects to the BinTrac and runs a continuous connection
    monitor loop. Every 5 seconds it checks the TCP socket is still open and
    does a small Modbus read to verify the device is responding. Automatically
    reconnects if the connection drops.
    """
    log("=" * 60)
    log("BinTrac Modbus TCP Test")
    log("=" * 60)
    log()

    # Open TCP connection to the BinTrac
    log(f"{ts()} Connecting to {HOST}:{PORT}...")
    client = ModbusTcpClient(HOST, port=PORT, timeout=5)

    if not client.connect():
        log(f"{ts()} ERROR: Could not connect to device")
        sys.exit(1)

    log(f"{ts()} Connected!")
    log()

    # Continuous connection health check loop
    connection_flag = 1    
    while connection_flag != 0:
        # First check if the underlying TCP socket is still open
        if not client.is_socket_open():
            log(f"{ts()} Connection lost! Reconnecting...")
            if not client.connect():
                log(f"{ts()} ERROR: Could not reconnect")
                time.sleep(5)
                continue
            log(f"{ts()} Reconnected!")

        # Do a lightweight read (2 registers = 1 bin) to confirm the device
        # is actually responding, not just that the socket is open
        try:
            result = client.read_input_registers(ALL_BINS_ADDR - 1, count=2, device_id=DEVICE_ID)
        except Exception as e:
            log(f"{ts()} Connection check failed: {e}")
            time.sleep(5)
            continue

        if result.isError():
            log(f"{ts()} Device not responding: {result}")
            time.sleep(5)
            continue

        log(f"{ts()} Modbus connection OK")
        connection_flag = connection_flag - 1
        time.sleep(5)

    # Continuous weight reading loop with change tracking and lb/min rate
    log(f"{ts()} Reading bin weights (Ctrl+C to stop)...")
    log("    Time |    A:    +/-   lb/min |    B:    +/-   lb/min |    C:    +/-   lb/min |    D:    +/-   lb/min | Total:   +/-   lb/min")
    log("-" * 126)

    prev_weights = None
    prev_time = None
    current_csv_date = None

    # Exponential moving average smoothing factor.
    # Lower = smoother (0.1 = heavy smoothing, 1.0 = no smoothing).
    ema_alpha = 0.3

    # Current smoothed weights (initialized on first reading)
    smoothed = None

    try:
        while True:
            result = client.read_input_registers(ALL_BINS_ADDR - 1, count=8, device_id=DEVICE_ID)
            if result.isError():
                log(f"{ts()} Read error: {result}")
                time.sleep(3)
                continue

            now = time.time()
            now_dt = datetime.now()
            reading = [
                parse_bin_weight(result.registers, 0),
                parse_bin_weight(result.registers, 2),
                parse_bin_weight(result.registers, 4),
                parse_bin_weight(result.registers, 6),
            ]

            if smoothed is None:
                # First reading - seed the EMA with raw values
                smoothed = list(reading)
            else:
                # Blend new reading into smoothed values
                for i in range(4):
                    smoothed[i] = ema_alpha * reading[i] + (1 - ema_alpha) * smoothed[i]

            weights = list(smoothed)
            weights.append(sum(weights))  # Total
            labels = ["A", "B", "C", "D", "Total"]

            if prev_weights is None:
                # First reading - no change data yet
                parts = []
                for i in range(len(labels)):
                    parts.append(f"{weights[i]:>7.0f}    --      --")
                log(ts() + " | " + " | ".join(parts))
            else:
                elapsed = now - prev_time
                parts = []
                for i in range(len(labels)):
                    change = weights[i] - prev_weights[i]
                    lb_min = change / elapsed * 60
                    parts.append(f"{weights[i]:>7.0f} {change:>+5.0f} {lb_min:>+7.1f}")
                log(ts() + " | " + " | ".join(parts))

            # CSV logging
            if _csv["writer"] is not None:
                today = date.today()
                if today != current_csv_date:
                    # Day rolled over - open new file, purge old ones
                    _csv["file"].close()
                    open_csv_for_today()
                    current_csv_date = today
                    purge_old_csvs()
                _csv["writer"].writerow([
                    now_dt.strftime("%Y-%m-%d %H:%M:%S"),
                    f"{weights[0]:.1f}",
                    f"{weights[1]:.1f}",
                    f"{weights[2]:.1f}",
                    f"{weights[3]:.1f}",
                    f"{weights[4]:.1f}",
                ])
                _csv["file"].flush()

            prev_weights = weights
            prev_time = now
            time.sleep(3)
    finally:
        client.close()
        if _csv["file"]:
            _csv["file"].close()
        if _log_file:
            _log_file.close()
            print(f"Session saved to {_log_file.name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="BinTrac Modbus TCP weight monitor")
    parser.add_argument("--record", action="store_true", help="Record session to a timestamped .txt file")
    parser.add_argument("--csv", action="store_true", help="Log weights to daily CSV files in weight_logs/")
    args = parser.parse_args()

    if args.record:
        filename = f"Weight_Session_{datetime.now().strftime('%Y%m%d_%H%M%S')}.txt"
        _log_file = open(filename, "w")
        print(f"Recording to {filename}")

    if args.csv:
        purge_old_csvs()
        open_csv_for_today()

    main()
