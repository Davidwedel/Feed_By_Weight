#!/usr/bin/env python3
"""
Modbus TCP test script for BinTrac HouseLink
Tests reading bin weights from the device

Requirements: pip install pymodbus
"""

import sys
import time

from pymodbus.client import ModbusTcpClient

# Configuration
HOST = "192.168.1.173"  # BinTrac IP
PORT = 502          # Modbus TCP port
DEVICE_ID = 1       # Unit ID (Station ID from HL-10E Devices page)

# Modbus addresses (from docs sample - address 999 in protocol, displays as 1000)
BIN_A_ADDR = 1000
BIN_B_ADDR = 1002
BIN_C_ADDR = 1004
BIN_D_ADDR = 1006
ALL_BINS_ADDR = 1000


def parse_bin_weight(registers, offset=0):
    """Parse weight from register pair (HouseLink format - 32-bit signed)"""
    if offset + 1 >= len(registers):
        return None

    # Each bin is 32-bit signed integer stored in 2 registers (big-endian)
    high_word = registers[offset]
    low_word = registers[offset + 1]

    # Combine into 32-bit value
    raw_value = (high_word << 16) | low_word

    # Convert to signed 32-bit
    if raw_value > 2147483647:
        raw_value = raw_value - 4294967296

    # Check for disabled bin marker
    if raw_value == -32767 or raw_value == 0xFFFF8001:
        return 0.0

    return float(raw_value)


def read_registers(client, start_addr, num_registers):
    """Read input registers using pymodbus"""
    # pymodbus uses 0-indexed addresses
    protocol_addr = start_addr - 1

    print(f"Reading {num_registers} registers from address {start_addr} (protocol addr: {protocol_addr})")

    result = client.read_input_registers(protocol_addr, count=num_registers, device_id=DEVICE_ID)

    if result.isError():
        print(f"ERROR: Modbus error - {result}")
        return None

    print(f"Read {len(result.registers)} registers: {result.registers}")
    return result.registers


def print_weights(registers):
    """Parse and print bin weights from registers"""
    bin_a = parse_bin_weight(registers, 0)
    bin_b = parse_bin_weight(registers, 2)
    bin_c = parse_bin_weight(registers, 4)
    bin_d = parse_bin_weight(registers, 6)
    print(f"  Bin A: {bin_a} lbs")
    print(f"  Bin B: {bin_b} lbs")
    print(f"  Bin C: {bin_c} lbs")
    print(f"  Bin D: {bin_d} lbs")
    print(f"  Total: {bin_a + bin_b + bin_c + bin_d} lbs")


def scan_device_ids(client):
    """Scan for working device IDs"""
    print("Scanning for device IDs (1-20)...")
    print("This will try common BinTrac station IDs")
    print("-" * 60)
    for test_id in range(1, 21):
        print(f"\nTrying device ID {test_id}...")
        try:
            result = client.read_input_registers(ALL_BINS_ADDR - 1, count=2, device_id=test_id)

            if not result.isError():
                print(f"\n{'='*60}")
                print(f"SUCCESS! Device ID {test_id} works!")
                print(f"{'='*60}")
                print(f"\nUpdate DEVICE_ID in the script to: {test_id}")
                return
        except Exception as e:
            print(f"  Error: {e}")
        time.sleep(0.5)

    print("\nNo working device ID found in range 1-20")
    print("Check the 'Devices' page on the HL-10E web interface to see station IDs")


def main():
    print("=" * 60)
    print("BinTrac Modbus TCP Test")
    print("=" * 60)
    print()

    print(f"Connecting to {HOST}:{PORT}...")
    client = ModbusTcpClient(HOST, port=PORT, timeout=5)

    if not client.connect():
        print("ERROR: Could not connect to device")
        sys.exit(1)

    print("Connected!")
    print()

    while True:
        if not client.is_socket_open():
            print("Connection lost! Reconnecting...")
            if not client.connect():
                print("ERROR: Could not reconnect")
                time.sleep(5)
                continue
            print("Reconnected!")

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
        time.sleep(5)

    try:
        if len(sys.argv) > 1 and sys.argv[1] == "--scan":
            scan_device_ids(client)
            return

        # Test 1: Read all bins
        print("Test 1: Reading all bins A, B, C, D (8 registers from address 1000)")
        print("-" * 60)
        registers = read_registers(client, ALL_BINS_ADDR, 8)

        if registers:
            print()
            print("Parsed weights:")
            print_weights(registers)

        print()
        print("-" * 60)

        # Test 2: Read single bin
        print("Test 2: Reading bin A only (2 registers from address 1000)")
        print("-" * 60)
        registers = read_registers(client, BIN_A_ADDR, 2)

        if registers:
            print()
            print("Parsed weight:")
            bin_a = parse_bin_weight(registers, 0)
            print(f"  Bin A: {bin_a} lbs")

        print()
        print("=" * 60)
        print("Test complete")
        print("=" * 60)
        print()
        print("If tests failed, try: python test_modbus.py --scan")

    finally:
        client.close()


if __name__ == "__main__":
    main()
