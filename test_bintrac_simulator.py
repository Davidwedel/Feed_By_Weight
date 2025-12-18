#!/usr/bin/env python3
"""
BinTrac HouseLink Modbus TCP Simulator
Simulates 4-bin weight monitoring system for testing ESP32 weight feeder.

Protocol:
- Modbus TCP on port 502
- Function Code 4 (Read Input Registers)
- Registers: 1000-1001 (Bin A), 1002-1003 (Bin B), 1004-1005 (Bin C), 1006-1007 (Bin D)
- Each bin: 2 registers, weight stored as signed 16-bit in first register
- Value -32767 = bin disabled
"""

import tkinter as tk
from tkinter import ttk, scrolledtext
import threading
import socket
import struct
import datetime

# Modbus TCP constants
MODBUS_PORT = 502
REGISTER_BASE = 1000  # Starting address for Bin A


class ModbusTCPServer:
    """Simple Modbus TCP server for Function Code 4 (Read Input Registers)"""

    def __init__(self, port, get_weights_callback):
        self.port = port
        self.get_weights = get_weights_callback
        self.running = False
        self.server_socket = None
        self.thread = None

    def start(self):
        """Start the Modbus TCP server"""
        self.running = True
        self.thread = threading.Thread(target=self._run_server, daemon=True)
        self.thread.start()

    def stop(self):
        """Stop the Modbus TCP server"""
        self.running = False
        if self.server_socket:
            self.server_socket.close()

    def _run_server(self):
        """Server main loop"""
        try:
            self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.server_socket.bind(('0.0.0.0', self.port))
            self.server_socket.listen(5)
            self.server_socket.settimeout(1.0)
            print(f"Modbus TCP server listening on port {self.port}")

            while self.running:
                try:
                    client_socket, address = self.server_socket.accept()
                    threading.Thread(target=self._handle_client, args=(client_socket, address), daemon=True).start()
                except socket.timeout:
                    continue
                except Exception as e:
                    if self.running:
                        print(f"Server error: {e}")
                    break
        except Exception as e:
            print(f"Failed to start server: {e}")

    def _handle_client(self, client_socket, address):
        """Handle a client connection"""
        try:
            client_socket.settimeout(5.0)

            # Read Modbus TCP request (12 bytes)
            request = client_socket.recv(12)
            if len(request) < 12:
                return

            # Parse request
            transaction_id = struct.unpack('>H', request[0:2])[0]
            protocol_id = struct.unpack('>H', request[2:4])[0]
            length = struct.unpack('>H', request[4:6])[0]
            unit_id = request[6]
            function_code = request[7]
            start_address = struct.unpack('>H', request[8:10])[0]
            register_count = struct.unpack('>H', request[10:12])[0]

            # Log request
            timestamp = datetime.datetime.now().strftime("%H:%M:%S")
            log_msg = f"[{timestamp}] {address[0]} - FC{function_code} addr={start_address} count={register_count}"
            print(log_msg)

            # Only support Function Code 4 (Read Input Registers)
            if function_code != 4:
                response = self._build_error_response(transaction_id, unit_id, function_code, 1)
                client_socket.send(response)
                return

            # Get current weights from GUI
            weights = self.get_weights()

            # Build response data (2 registers per bin, weight in first register)
            response_data = bytearray()
            for i in range(register_count):
                register_address = start_address + i
                bin_index = (register_address - REGISTER_BASE) // 2
                register_offset = (register_address - REGISTER_BASE) % 2

                if bin_index < 4 and register_offset == 0:
                    # First register of bin pair - contains weight
                    weight = weights[bin_index]
                    response_data.extend(struct.pack('>h', weight))  # Signed 16-bit
                else:
                    # Second register of pair or out of range - send 0
                    response_data.extend(struct.pack('>H', 0))

            # Build Modbus TCP response
            byte_count = len(response_data)
            response = struct.pack('>HHHBB',
                                 transaction_id,
                                 protocol_id,
                                 byte_count + 3,  # Unit ID + Function Code + Byte Count
                                 unit_id,
                                 function_code) + bytes([byte_count]) + response_data

            client_socket.send(response)

        except Exception as e:
            print(f"Client handler error: {e}")
        finally:
            client_socket.close()

    def _build_error_response(self, transaction_id, unit_id, function_code, exception_code):
        """Build Modbus error response"""
        return struct.pack('>HHHBBB',
                         transaction_id,
                         0,  # Protocol ID
                         3,  # Length
                         unit_id,
                         function_code | 0x80,  # Error flag
                         exception_code)


class BinTracSimulator:
    """GUI application for BinTrac simulator"""

    def __init__(self, root):
        self.root = root
        self.root.title("BinTrac HouseLink Simulator")
        self.root.geometry("800x700")

        # Bin weights (use signed 16-bit range)
        self.weights = [0, 0, 0, 0]
        self.enabled = [True, True, True, True]

        # Create GUI
        self._create_gui()

        # Start Modbus server
        self.server = ModbusTCPServer(MODBUS_PORT, self._get_weights)
        try:
            self.server.start()
            self.status_label.config(text="Server Status: Running on port 502", foreground="green")
        except Exception as e:
            self.status_label.config(text=f"Server Status: Failed - {e}", foreground="red")

    def _create_gui(self):
        """Create the GUI elements"""
        # Title
        title = tk.Label(self.root, text="BinTrac HouseLink Simulator", font=("Arial", 16, "bold"))
        title.pack(pady=10)

        # Connection info
        info_frame = tk.Frame(self.root)
        info_frame.pack(pady=5)

        tk.Label(info_frame, text="Configure ESP32 to connect to:", font=("Arial", 10)).pack()
        ip_label = tk.Label(info_frame, text=self._get_local_ip(), font=("Arial", 12, "bold"), foreground="blue")
        ip_label.pack()
        tk.Label(info_frame, text="Port: 502", font=("Arial", 10)).pack()

        self.status_label = tk.Label(self.root, text="Server Status: Starting...", foreground="orange")
        self.status_label.pack(pady=5)

        # Bins frame
        bins_frame = tk.Frame(self.root)
        bins_frame.pack(pady=10, padx=20, fill=tk.BOTH, expand=True)

        bin_labels = ['Bin A', 'Bin B', 'Bin C', 'Bin D']

        for i, label in enumerate(bin_labels):
            self._create_bin_control(bins_frame, i, label)

        # Total weight display
        total_frame = tk.Frame(self.root, relief=tk.RIDGE, borderwidth=2)
        total_frame.pack(pady=10, padx=20, fill=tk.X)

        tk.Label(total_frame, text="Total Weight:", font=("Arial", 12, "bold")).pack(side=tk.LEFT, padx=10)
        self.total_label = tk.Label(total_frame, text="0 lbs", font=("Arial", 14, "bold"), foreground="blue")
        self.total_label.pack(side=tk.LEFT, padx=10)

        # Log area
        log_frame = tk.LabelFrame(self.root, text="Modbus Request Log", font=("Arial", 10, "bold"))
        log_frame.pack(pady=10, padx=20, fill=tk.BOTH, expand=True)

        self.log_text = scrolledtext.ScrolledText(log_frame, height=8, state='disabled')
        self.log_text.pack(fill=tk.BOTH, expand=True)

        # Redirect print to log window
        import sys
        sys.stdout = TextRedirector(self.log_text)

    def _create_bin_control(self, parent, index, label):
        """Create controls for a single bin"""
        frame = tk.LabelFrame(parent, text=label, font=("Arial", 10, "bold"))
        frame.pack(side=tk.LEFT, padx=10, pady=5, fill=tk.BOTH, expand=True)

        # Enable/Disable checkbox
        enabled_var = tk.BooleanVar(value=True)
        enabled_check = tk.Checkbutton(frame, text="Enabled", variable=enabled_var,
                                      command=lambda: self._toggle_bin(index, enabled_var.get()))
        enabled_check.pack(pady=5)

        # Weight display
        weight_display = tk.Label(frame, text="0 lbs", font=("Arial", 16, "bold"), foreground="green")
        weight_display.pack(pady=5)

        # Entry field
        entry_frame = tk.Frame(frame)
        entry_frame.pack(pady=5)
        tk.Label(entry_frame, text="Set:").pack(side=tk.LEFT)
        entry = tk.Entry(entry_frame, width=8)
        entry.pack(side=tk.LEFT, padx=5)
        entry.insert(0, "0")
        set_btn = tk.Button(entry_frame, text="Set",
                           command=lambda: self._set_weight(index, entry, weight_display))
        set_btn.pack(side=tk.LEFT)

        # +/- 10 buttons
        large_frame = tk.Frame(frame)
        large_frame.pack(pady=5)
        tk.Button(large_frame, text="-10", width=5,
                 command=lambda: self._adjust_weight(index, -10, weight_display)).pack(side=tk.LEFT, padx=2)
        tk.Button(large_frame, text="+10", width=5,
                 command=lambda: self._adjust_weight(index, 10, weight_display)).pack(side=tk.LEFT, padx=2)

        # +/- 1 buttons
        small_frame = tk.Frame(frame)
        small_frame.pack(pady=5)
        tk.Button(small_frame, text="-1", width=5,
                 command=lambda: self._adjust_weight(index, -1, weight_display)).pack(side=tk.LEFT, padx=2)
        tk.Button(small_frame, text="+1", width=5,
                 command=lambda: self._adjust_weight(index, 1, weight_display)).pack(side=tk.LEFT, padx=2)

    def _toggle_bin(self, index, enabled):
        """Toggle bin enabled/disabled"""
        self.enabled[index] = enabled
        if not enabled:
            self.weights[index] = -32767  # Disabled marker
        else:
            self.weights[index] = 0
        self._update_total()

    def _set_weight(self, index, entry, display):
        """Set weight from entry field"""
        try:
            value = int(entry.get())
            # Clamp to signed 16-bit range
            value = max(-32767, min(32767, value))
            self.weights[index] = value
            display.config(text=f"{value} lbs")
            self._update_total()
        except ValueError:
            pass

    def _adjust_weight(self, index, delta, display):
        """Adjust weight by delta"""
        if not self.enabled[index]:
            return
        new_value = self.weights[index] + delta
        # Clamp to signed 16-bit range
        new_value = max(-32767, min(32767, new_value))
        self.weights[index] = new_value
        display.config(text=f"{new_value} lbs")
        self._update_total()

    def _update_total(self):
        """Update total weight display"""
        total = sum(w for w, e in zip(self.weights, self.enabled) if e and w != -32767)
        self.total_label.config(text=f"{total} lbs")

    def _get_weights(self):
        """Get current weights for Modbus server (callback)"""
        return self.weights.copy()

    def _get_local_ip(self):
        """Get local IP address"""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except:
            return "127.0.0.1"


class TextRedirector:
    """Redirect stdout to tkinter Text widget"""

    def __init__(self, text_widget):
        self.text_widget = text_widget

    def write(self, string):
        self.text_widget.config(state='normal')
        self.text_widget.insert(tk.END, string)
        self.text_widget.see(tk.END)
        self.text_widget.config(state='disabled')

    def flush(self):
        pass


def main():
    root = tk.Tk()
    app = BinTracSimulator(root)
    root.mainloop()


if __name__ == "__main__":
    main()
