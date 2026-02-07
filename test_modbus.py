#!/usr/bin/env python3
"""
Modbus TCP test script for BinTrac HouseLink HL-10E.
Connects to the BinTrac over TCP and reads bin weight values using the
Modbus protocol (function code 4 - Read Input Registers).

Usage:
  python test_modbus.py          # Continuous connection monitor (prints OK every 5s)

Requirements: pip install pymodbus
"""

import sys
import time

from pymodbus.client import ModbusTcpClient

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

    print(f"Reading {num_registers} registers from address {start_addr} (protocol addr: {protocol_addr})")

    result = client.read_input_registers(protocol_addr, count=num_registers, device_id=DEVICE_ID)

    if result.isError():
        print(f"ERROR: Modbus error - {result}")
        return None

    print(f"Read {len(result.registers)} registers: {result.registers}")
    return result.registers


def print_weights(registers):
    """Parse all 4 bins from an 8-register read and print their weights."""
    bin_a = parse_bin_weight(registers, 0)
    bin_b = parse_bin_weight(registers, 2)
    bin_c = parse_bin_weight(registers, 4)
    bin_d = parse_bin_weight(registers, 6)
    print(f"  Bin A: {bin_a} lbs")
    print(f"  Bin B: {bin_b} lbs")
    print(f"  Bin C: {bin_c} lbs")
    print(f"  Bin D: {bin_d} lbs")
    print(f"  Total: {bin_a + bin_b + bin_c + bin_d} lbs")


def main():
    """Entry point. Connects to the BinTrac and runs a continuous connection
    monitor loop. Every 5 seconds it checks the TCP socket is still open and
    does a small Modbus read to verify the device is responding. Automatically
    reconnects if the connection drops.
    """
    print("=" * 60)
    print("BinTrac Modbus TCP Test")
    print("=" * 60)
    print()

    # Open TCP connection to the BinTrac
    print(f"Connecting to {HOST}:{PORT}...")
    client = ModbusTcpClient(HOST, port=PORT, timeout=5)

    if not client.connect():
        print("ERROR: Could not connect to device")
        sys.exit(1)

    print("Connected!")
    print()

    # Continuous connection health check loop
    connection_flag = 2    
    while connection_flag != 0:
        # First check if the underlying TCP socket is still open
        if not client.is_socket_open():
            print("Connection lost! Reconnecting...")
            if not client.connect():
                print("ERROR: Could not reconnect")
                time.sleep(5)
                continue
            print("Reconnected!")

        # Do a lightweight read (2 registers = 1 bin) to confirm the device
        # is actually responding, not just that the socket is open
        try:
            result = client.read_input_registers(ALL_BINS_ADDR - 1, count=2, device_id=DEVICE_ID)
        except Exception as e:
            print(f"Connection check failed: {e}")
            time.sleep(5)
            continue

        if result.isError():
            print(f"Device not responding: {result}")
            time.sleep(5)
            continue

        print("Modbus connection OK")
        connection_flag = connection_flag - 1
        time.sleep(5)

    # Continuous weight reading loop - prints all bins on one line every second
    print("Reading bin weights (Ctrl+C to stop)...")
    print("-" * 60)
    try:
        while True:
            result = client.read_input_registers(ALL_BINS_ADDR - 1, count=8, device_id=DEVICE_ID)
            if result.isError():
                print(f"Read error: {result}")
            else:
                a = parse_bin_weight(result.registers, 0)
                b = parse_bin_weight(result.registers, 2)
                c = parse_bin_weight(result.registers, 4)
                d = parse_bin_weight(result.registers, 6)
                total = a + b + c + d
                print(f"A: {a}  B: {b}  C: {c}  D: {d}  Total: {total} lbs")
            time.sleep(3)
    finally:
        client.close()


if __name__ == "__main__":
    main()
