#!/usr/bin/env python3
"""
Simple Modbus TCP test script for BinTrac HouseLink
Tests reading bin weights from the device
"""

import struct
import socket
import sys

# Configuration
HOST = "10.0.0.35"  # BinTrac IP
PORT = 502          # Modbus TCP port
DEVICE_ID = 1       # Unit ID

# Modbus addresses (from your config)
BIN_A_ADDR = 1000
BIN_B_ADDR = 1002
BIN_C_ADDR = 1004
BIN_D_ADDR = 1006
ALL_BINS_ADDR = 1000

def read_modbus_registers(host, port, device_id, start_addr, num_registers):
    """Read input registers using Modbus TCP Function Code 4"""

    # Create TCP connection
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5.0)

    try:
        print(f"Connecting to {host}:{port}...")
        sock.connect((host, port))
        print("Connected!")

        # Build Modbus TCP request
        transaction_id = 1
        protocol_id = 0
        length = 6  # Remaining bytes
        function_code = 4  # Read Input Registers

        # Pack request (MBAP header + PDU)
        request = struct.pack(
            '>HHHBBHH',
            transaction_id,
            protocol_id,
            length,
            device_id,
            function_code,
            start_addr,
            num_registers
        )

        print(f"Sending request: Read {num_registers} registers from address {start_addr}")
        sock.send(request)

        # Read response header (9 bytes minimum)
        header = sock.recv(9)
        if len(header) < 9:
            print(f"ERROR: Incomplete header ({len(header)} bytes)")
            return None

        trans_id, proto_id, resp_length, unit_id, func_code, byte_count = struct.unpack('>HHHBBB', header)

        print(f"Response: trans_id={trans_id}, func_code={func_code}, byte_count={byte_count}")

        # Check for error response
        if func_code & 0x80:
            exception = byte_count  # In error response, this is the exception code
            print(f"ERROR: Modbus exception code {exception}")
            return None

        # Read data bytes
        data = sock.recv(byte_count)
        if len(data) < byte_count:
            print(f"ERROR: Incomplete data ({len(data)} of {byte_count} bytes)")
            return None

        # Parse register values (big-endian 16-bit)
        registers = []
        for i in range(0, len(data), 2):
            value = struct.unpack('>H', data[i:i+2])[0]
            registers.append(value)

        print(f"Registers: {registers}")
        return registers

    except socket.timeout:
        print(f"ERROR: Connection timeout")
        return None
    except ConnectionRefusedError:
        print(f"ERROR: Connection refused - is the device accessible?")
        return None
    except Exception as e:
        print(f"ERROR: {e}")
        return None
    finally:
        sock.close()

def parse_bin_weight(registers, offset=0):
    """Parse weight from register pair (HouseLink format)"""
    if offset >= len(registers):
        return None

    # HouseLink uses only the first register (16-bit signed)
    raw_value = registers[offset]

    # Convert to signed 16-bit
    if raw_value > 32767:
        raw_value = raw_value - 65536

    # Check for disabled bin marker
    if raw_value == -32767:
        return 0.0

    return float(raw_value)

def main():
    print("=" * 60)
    print("BinTrac Modbus TCP Test")
    print("=" * 60)
    print()

    # Test 1: Read all bins (6 registers = bins A, B, C)
    print("Test 1: Reading bins A, B, C (6 registers from address 1000)")
    print("-" * 60)
    registers = read_modbus_registers(HOST, PORT, DEVICE_ID, ALL_BINS_ADDR, 6)

    if registers:
        print()
        print("Parsed weights:")
        bin_a = parse_bin_weight(registers, 0)
        bin_b = parse_bin_weight(registers, 2)
        bin_c = parse_bin_weight(registers, 4)
        print(f"  Bin A: {bin_a} lbs")
        print(f"  Bin B: {bin_b} lbs")
        print(f"  Bin C: {bin_c} lbs")

    print()
    print("-" * 60)

    # Test 2: Read bin D separately
    print("Test 2: Reading bin D (2 registers from address 1006)")
    print("-" * 60)
    registers = read_modbus_registers(HOST, PORT, DEVICE_ID, BIN_D_ADDR, 2)

    if registers:
        print()
        print("Parsed weight:")
        bin_d = parse_bin_weight(registers, 0)
        print(f"  Bin D: {bin_d} lbs")

    print()
    print("=" * 60)
    print("Test complete")
    print("=" * 60)

if __name__ == "__main__":
    main()
