#!/usr/bin/env python3
"""
Simple Modbus TCP test script for BinTrac HouseLink
Tests reading bin weights from the device

Requirements: pip install pymodbus
"""

import struct
import socket
import sys
import time

# Try to use pymodbus library if available
try:
    from pymodbus.client import ModbusTcpClient
    PYMODBUS_AVAILABLE = True
except ImportError:
    PYMODBUS_AVAILABLE = False
    print("WARNING: pymodbus not available, using raw socket implementation")
    print("Install with: pip install pymodbus")
    print()

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

def read_modbus_registers(host, port, device_id, start_addr, num_registers, adjust_addr=True):
    """Read input registers using Modbus TCP Function Code 4

    Args:
        adjust_addr: If True, subtract 1 from address (Modbus 0-indexed addressing)
    """

    # Create TCP connection
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5.0)
    # Disable Nagle's algorithm for immediate sending
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    try:
        print(f"Connecting to {host}:{port}...")
        sock.connect((host, port))
        print("Connected!")

        # Adjust address for Modbus protocol (0-indexed vs 1-indexed)
        protocol_addr = start_addr - 1 if adjust_addr else start_addr

        # Build Modbus TCP request
        transaction_id = 1  # Simple transaction ID
        protocol_id = 0
        length = 6  # Remaining bytes after this field
        function_code = 4  # Read Input Registers

        # Pack request (MBAP header + PDU)
        # Format: trans_id(H) proto_id(H) length(H) device_id(B) func(B) addr(H) count(H)
        request = struct.pack(
            '>HHHBBHH',
            transaction_id,
            protocol_id,
            length,
            device_id,
            function_code,
            protocol_addr,
            num_registers
        )

        print(f"Sending request: Read {num_registers} registers from address {start_addr} (protocol addr: {protocol_addr})")
        print(f"Request hex: {request.hex()}")

        # Send all at once
        total_sent = 0
        while total_sent < len(request):
            sent = sock.send(request[total_sent:])
            if sent == 0:
                raise RuntimeError("Socket connection broken")
            total_sent += sent

        print(f"Sent {total_sent} bytes successfully")

        # Read response header (9 bytes minimum)
        print("Waiting for response...")
        header = sock.recv(9)
        if len(header) < 9:
            print(f"ERROR: Incomplete header ({len(header)} bytes)")
            return None

        trans_id, proto_id, resp_length, unit_id, func_code, byte_count = struct.unpack('>HHHBBB', header)

        print(f"Response header: {header.hex()}")
        print(f"Response: trans_id={trans_id:04x}, proto_id={proto_id}, length={resp_length}, unit_id={unit_id}, func_code={func_code:02x}, byte_count={byte_count}")

        # Check for error response (function code has high bit set)
        if func_code & 0x80:
            exception = byte_count  # In error response, this is the exception code
            error_msgs = {
                0x01: "Modbus pointer error (should be function code 04)",
                0x02: "Out of bounds address (should be 1000-1016, 1200-1236, or 1400-1436)",
                0x03: "Out of bounds address (should be 1000-1016, 1200-1236, or 1400-1436)",
                0x0A: "Discovery error - device not discovered properly or wiring issue",
                0x0B: "Communication error with BinTrac - check wiring and station ID"
            }
            error_msg = error_msgs.get(exception, f"Unknown error code")
            print(f"ERROR: Modbus exception 0x{exception:02x}: {error_msg}")
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

def read_with_pymodbus(host, port, device_id, start_addr, num_registers):
    """Try reading using pymodbus library"""
    if not PYMODBUS_AVAILABLE:
        return None

    print(f"Attempting connection with pymodbus library...")
    client = ModbusTcpClient(host, port=port, timeout=5)

    try:
        if not client.connect():
            print("ERROR: Could not connect to device")
            return None

        print(f"Connected! Reading {num_registers} registers from address {start_addr - 1}")

        # pymodbus 3.x uses 'unit' instead of 'slave'
        # Try both for compatibility
        try:
            result = client.read_input_registers(start_addr - 1, num_registers, unit=device_id)
        except TypeError:
            result = client.read_input_registers(start_addr - 1, num_registers, slave=device_id)

        if result.isError():
            print(f"ERROR: Modbus error - {result}")
            return None

        print(f"SUCCESS! Read {len(result.registers)} registers")
        return result.registers

    except Exception as e:
        print(f"ERROR: {e}")
        return None
    finally:
        client.close()

def main():
    print("=" * 60)
    print("BinTrac Modbus TCP Test")
    print("=" * 60)
    print()

    # Try pymodbus first if available
    if PYMODBUS_AVAILABLE and len(sys.argv) > 1 and sys.argv[1] == "--pymodbus":
        print("Using pymodbus library")
        print("-" * 60)
        registers = read_with_pymodbus(HOST, PORT, DEVICE_ID, ALL_BINS_ADDR, 8)
        if registers:
            print()
            print("Parsed weights:")
            bin_a = parse_bin_weight(registers, 0)
            bin_b = parse_bin_weight(registers, 2)
            bin_c = parse_bin_weight(registers, 4)
            bin_d = parse_bin_weight(registers, 6)
            print(f"  Bin A: {bin_a} lbs")
            print(f"  Bin B: {bin_b} lbs")
            print(f"  Bin C: {bin_c} lbs")
            print(f"  Bin D: {bin_d} lbs")
            print(f"  Total: {bin_a + bin_b + bin_c + bin_d} lbs")
        return

    # Try to detect the correct device ID first
    if len(sys.argv) > 1 and sys.argv[1] == "--scan":
        print("Scanning for device IDs (1-20)...")
        print("This will try common BinTrac station IDs")
        print("-" * 60)
        for test_id in range(1, 21):
            print(f"\nTrying device ID {test_id}...")
            registers = read_modbus_registers(HOST, PORT, test_id, ALL_BINS_ADDR, 2)
            if registers:
                print(f"\n{'='*60}")
                print(f"SUCCESS! Device ID {test_id} works!")
                print(f"{'='*60}")
                print(f"\nUpdate DEVICE_ID in the script to: {test_id}")
                return
            time.sleep(0.5)  # Delay between attempts
        print("\nNo working device ID found in range 1-20")
        print("Check the 'Devices' page on the HL-10E web interface to see station IDs")
        return

    # Test 1: Read all bins with address adjustment (protocol addr 999)
    print("Test 1: Reading all bins A, B, C, D (8 registers from address 1000)")
    print("-" * 60)
    registers = read_modbus_registers(HOST, PORT, DEVICE_ID, ALL_BINS_ADDR, 8, adjust_addr=True)

    if registers:
        print()
        print("Parsed weights:")
        bin_a = parse_bin_weight(registers, 0)
        bin_b = parse_bin_weight(registers, 2)
        bin_c = parse_bin_weight(registers, 4)
        bin_d = parse_bin_weight(registers, 6)
        print(f"  Bin A: {bin_a} lbs")
        print(f"  Bin B: {bin_b} lbs")
        print(f"  Bin C: {bin_c} lbs")
        print(f"  Bin D: {bin_d} lbs")
        print(f"  Total: {bin_a + bin_b + bin_c + bin_d} lbs")
    else:
        # Try without address adjustment
        print("\nRetrying without address adjustment (protocol addr 1000)...")
        registers = read_modbus_registers(HOST, PORT, DEVICE_ID, ALL_BINS_ADDR, 8, adjust_addr=False)

        if registers:
            print()
            print("Parsed weights:")
            bin_a = parse_bin_weight(registers, 0)
            bin_b = parse_bin_weight(registers, 2)
            bin_c = parse_bin_weight(registers, 4)
            bin_d = parse_bin_weight(registers, 6)
            print(f"  Bin A: {bin_a} lbs")
            print(f"  Bin B: {bin_b} lbs")
            print(f"  Bin C: {bin_c} lbs")
            print(f"  Bin D: {bin_d} lbs")
            print(f"  Total: {bin_a + bin_b + bin_c + bin_d} lbs")

    print()
    print("-" * 60)

    # Test 2: Read single bin (for comparison)
    print("Test 2: Reading bin A only (2 registers from address 1000)")
    print("-" * 60)
    registers = read_modbus_registers(HOST, PORT, DEVICE_ID, BIN_A_ADDR, 2, adjust_addr=True)

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

if __name__ == "__main__":
    main()
