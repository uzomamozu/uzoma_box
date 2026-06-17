#!/usr/bin/env python3
"""
UzomaBox Desktop Controller
=============================
TCP client to control a Teensy 4.1 running UzomaBox.ino

No external dependencies — uses Python stdlib: tkinter, socket, threading.

Protocol (port 8888):
  Send:   PING                    → Receive: PONG
  Send:   MODE:artnet             → Receive: OK:<msg>
  Send:   MODE:playback           → Receive: OK:<msg>
  Send:   MODE:record             → Receive: OK:<msg>
  Send:   REC:START               → Receive: OK:<msg>
  Send:   REC:STOP                → Receive: OK:<msg>
  Send:   STATUS                  → Receive: mode=...\nip=...
  Send:   PLAY:<filename.BIN>     → Receive: OK:<msg>
  Send:   PLAY:SEQUENCE           → Receive: OK:<msg>
  Send:   STOP                    → Receive: OK:stopped
  Send:   CONFIG:key=value        → Receive: OK:<msg>  (reboots Teensy)
  Send:   CONFIG:key=val1,val2..  → Receive: OK:<msg>  (reboots Teensy)
"""

import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox
import socket
import threading
import time
import queue
from datetime import datetime

# ============================================================================
# Network interface enumeration (stdlib only)
# ============================================================================
def get_interfaces():
    """Return list of (ip, name) tuples for all non-loopback IPv4 interfaces."""
    interfaces = []
    try:
        for if_idx, if_name in socket.if_nameindex():
            try:
                addrs = socket.getaddrinfo(if_name, None, socket.AF_INET)
                for addr in addrs:
                    ip = addr[4][0]
                    # Skip loopback
                    if not ip.startswith("127."):
                        interfaces.append((ip, if_name))
                        break
            except (socket.gaierror, OSError):
                pass
    except Exception:
        # Fallback for older Python / platforms without if_nameindex:
        # Attempt to get host's IP via gethostbyname
        try:
            hostname = socket.gethostname()
            ip = socket.gethostbyname(hostname)
            if not ip.startswith("127."):
                interfaces.append((ip, hostname))
        except:
            pass
    # Deduplicate by IP (keep first occurrence)
    seen = set()
    unique = []
    for ip, name in interfaces:
        if ip not in seen:
            seen.add(ip)
            unique.append((ip, name))
    return unique

# ============================================================================
# Configuration defaults
# ============================================================================
DEFAULT_IP       = "192.168.0.211"
DEFAULT_PORT     = 8888
PING_INTERVAL    = 2.0      # seconds between PINGs
PING_TIMEOUT     = 1.0      # seconds to wait for PONG
MAX_RECONNECT_DELAY = 10.0  # cap reconnection backoff at this many seconds
STATUS_INTERVAL  = 5.0      # seconds between automatic STATUS requests

# ============================================================================
# TCP client (runs on a background thread)
# ============================================================================
class TcpClient:
    """Manages a single TCP connection to the Teensy."""

    def __init__(self, host, port, send_queue, recv_queue, log_callback, status_callback, bind_ip=None):
        self.host = host
        self.port = port
        self.send_queue = send_queue       # commands to send
        self.recv_queue = recv_queue       # responses received
        self.log_callback = log_callback
        self.status_callback = status_callback  # called with (connected, latency_ms)
        self.bind_ip = bind_ip             # optional IP to bind socket to

        self.sock = None
        self.running = False
        self.connected = False
        self.latency_ms = 0.0
        self.ping_count = 0
        self.server_banner = ""

    def start(self):
        self.running = True
        threading.Thread(target=self._run, daemon=True).start()

    def stop(self):
        self.running = False
        if self.sock:
            try:
                self.sock.close()
            except:
                pass

    def _run(self):
        reconnect_delay = 1.0
        last_ping = 0.0
        last_status = 0.0
        line_buffer = ""

        while self.running:
            # ---- Connect if not connected ----
            if not self.connected:
                try:
                    self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    self.sock.settimeout(5.0)

                    # Bind to a specific adapter if requested
                    if self.bind_ip:
                        self.sock.bind((self.bind_ip, 0))

                    self.sock.connect((self.host, self.port))
                    self.connected = True
                    self.ping_count = 0
                    self._log("CONNECTED to %s:%d" % (self.host, self.port))
                    self.status_callback(True, 0.0)
                    reconnect_delay = 1.0
                    # Drain the initial "OK:connected" banner
                    try:
                        banner = self.sock.recv(1024).decode("utf-8", errors="replace").strip()
                        self.server_banner = banner
                        self._log("← %s" % banner)
                    except:
                        pass
                except socket.timeout:
                    self._log("TIMEOUT connecting to %s:%d" % (self.host, self.port))
                    self.status_callback(False, 0.0)
                    time.sleep(reconnect_delay)
                    reconnect_delay = min(reconnect_delay * 2, MAX_RECONNECT_DELAY)
                    continue
                except OSError as e:
                    self._log("CONNECTION ERROR: %s" % str(e))
                    self.status_callback(False, 0.0)
                    time.sleep(reconnect_delay)
                    reconnect_delay = min(reconnect_delay * 2, MAX_RECONNECT_DELAY)
                    continue

            # ---- Send queued commands ----
            try:
                while not self.send_queue.empty():
                    cmd = self.send_queue.get_nowait()
                    self._send(cmd)
            except queue.Empty:
                pass

            # ---- PING keep-alive ----
            now = time.monotonic()
            if now - last_ping >= PING_INTERVAL:
                last_ping = now
                self._send("PING", expect_pong=True)

            # ---- Automatic STATUS ----
            if now - last_status >= STATUS_INTERVAL:
                last_status = now
                self._send("STATUS")

            # ---- Receive response ----
            try:
                data = self.sock.recv(4096).decode("utf-8", errors="replace")
                if not data:
                    raise ConnectionResetError("Empty read")
                line_buffer += data
                # Process complete lines
                while "\n" in line_buffer or "\r" in line_buffer:
                    idx = -1
                    for sep in ("\r\n", "\n", "\r"):
                        i = line_buffer.find(sep)
                        if i >= 0 and (idx < 0 or i < idx):
                            idx = i
                            sep_len = len(sep)
                    if idx < 0:
                        break
                    line = line_buffer[:idx]
                    line_buffer = line_buffer[idx + sep_len:]
                    self._process_line(line)
            except socket.timeout:
                pass  # normal, just loop again
            except (OSError, ConnectionResetError) as e:
                self._log("CONNECTION LOST: %s" % str(e))
                self.connected = False
                self.status_callback(False, 0.0)
                if self.sock:
                    try:
                        self.sock.close()
                    except:
                        pass
                    self.sock = None
                time.sleep(reconnect_delay)
                reconnect_delay = min(reconnect_delay * 2, MAX_RECONNECT_DELAY)

    def _send(self, cmd, expect_pong=False):
        if not self.connected or not self.sock:
            return
        try:
            if expect_pong:
                self._send_start = time.monotonic()
            self.sock.sendall((cmd + "\n").encode("utf-8"))
            self._log("→ %s" % cmd)
        except OSError as e:
            self._log("SEND ERROR: %s" % str(e))

    def _process_line(self, line):
        line = line.strip()
        if not line:
            return
        self._log("← %s" % line)

        # Check if this is a PONG response
        if line == "PONG":
            if hasattr(self, "_send_start"):
                elapsed = (time.monotonic() - self._send_start) * 1000
                self.latency_ms = round(elapsed, 1)
                self.ping_count = 0
                self.status_callback(True, self.latency_ms)
            return

        # Put response in queue for GUI processing
        self.recv_queue.put(line)

    def _log(self, msg):
        if self.log_callback:
            self.log_callback(msg)


# ============================================================================
# Main Application
# ============================================================================
class UzomaBoxApp:
    def __init__(self, root):
        self.root = root
        root.title("UzomaBox Desktop Controller")
        root.geometry("820x720")
        root.resizable(True, True)

        self.send_queue = queue.Queue()
        self.recv_queue = queue.Queue()

        self.client = None
        self.connected = False
        self._interface_map = {}  # maps combobox display string -> IP

        self._build_ui()
        self._poll_queue()

    # ---- UI BUILDING ------------------------------------------------------

    def _build_ui(self):
        main_frame = ttk.Frame(self.root, padding=10)
        main_frame.pack(fill=tk.BOTH, expand=True)

        # ---- Connection frame ---------------------------------------------
        conn_frame = ttk.LabelFrame(main_frame, text="Connection", padding=8)
        conn_frame.pack(fill=tk.X, pady=(0, 8))

        # Row 0
        ttk.Label(conn_frame, text="IP:").grid(row=0, column=0, sticky=tk.W, padx=(0,4))
        self.ip_var = tk.StringVar(value=DEFAULT_IP)
        ttk.Entry(conn_frame, textvariable=self.ip_var, width=18).grid(row=0, column=1, padx=(0,8))

        ttk.Label(conn_frame, text="Adapter:").grid(row=0, column=2, sticky=tk.W, padx=(8,4))
        self.iface_var = tk.StringVar()
        self.iface_combo = ttk.Combobox(conn_frame, textvariable=self.iface_var, width=26, state="readonly")
        self.iface_combo.grid(row=0, column=3, padx=(0,8))
        try:
            self._populate_interfaces()
        except Exception:
            # If interface enumeration fails, still show the dropdown with (auto)
            self.iface_combo["values"] = ["(auto)"]
            self.iface_var.set("(auto)")

        self.connect_btn = ttk.Button(conn_frame, text="Connect", command=self._on_connect)
        self.connect_btn.grid(row=0, column=4, padx=(0,4))
        self.disconnect_btn = ttk.Button(conn_frame, text="Disconnect", command=self._on_disconnect, state=tk.DISABLED)
        self.disconnect_btn.grid(row=0, column=5, padx=(0,12))

        self.conn_indicator = tk.Canvas(conn_frame, width=14, height=14, highlightthickness=0)
        self.conn_indicator.grid(row=0, column=6, padx=(0,4))
        self._set_conn_indicator("red")

        ttk.Label(conn_frame, text="Latency:").grid(row=0, column=7, sticky=tk.W, padx=(8,4))
        self.latency_var = tk.StringVar(value="-- ms")
        ttk.Label(conn_frame, textvariable=self.latency_var, width=8).grid(row=0, column=8, sticky=tk.W)

        # ---- Mode frame ---------------------------------------------------
        mode_frame = ttk.LabelFrame(main_frame, text="Mode", padding=8)
        mode_frame.pack(fill=tk.X, pady=(0, 8))

        self.mode_var = tk.StringVar(value="artnet")
        ttk.Radiobutton(mode_frame, text="ArtNet",  variable=self.mode_var,
                         value="artnet",  command=self._on_mode_change).pack(side=tk.LEFT, padx=(0,12))
        ttk.Radiobutton(mode_frame, text="Playback", variable=self.mode_var,
                         value="playback", command=self._on_mode_change).pack(side=tk.LEFT, padx=(0,12))
        ttk.Radiobutton(mode_frame, text="Record",  variable=self.mode_var,
                         value="record",  command=self._on_mode_change).pack(side=tk.LEFT, padx=(0,12))

        # ---- Action frame (Recording + Playback) --------------------------
        action_frame = ttk.Frame(main_frame)
        action_frame.pack(fill=tk.X, pady=(0, 8))

        # Recording sub-frame
        rec_frame = ttk.LabelFrame(action_frame, text="Recording", padding=8)
        rec_frame.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 4))

        self.rec_start_btn = ttk.Button(rec_frame, text="▶ Start",
                                         command=self._on_rec_start)
        self.rec_start_btn.pack(side=tk.LEFT, padx=(0, 6))
        self.rec_stop_btn = ttk.Button(rec_frame, text="■ Stop",
                                        command=self._on_rec_stop, state=tk.DISABLED)
        self.rec_stop_btn.pack(side=tk.LEFT)
        self.rec_status_var = tk.StringVar(value="Idle")
        ttk.Label(rec_frame, textvariable=self.rec_status_var).pack(side=tk.LEFT, padx=(12, 0))

        # Playback sub-frame
        play_frame = ttk.LabelFrame(action_frame, text="Playback", padding=8)
        play_frame.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(4, 0))

        self.play_file_var = tk.StringVar()
        ttk.Entry(play_frame, textvariable=self.play_file_var, width=18).pack(side=tk.LEFT, padx=(0, 6))
        self.play_btn = ttk.Button(play_frame, text="Play File", command=self._on_play_file)
        self.play_btn.pack(side=tk.LEFT, padx=(0, 4))
        self.play_all_btn = ttk.Button(play_frame, text="Play All", command=self._on_play_sequence)
        self.play_all_btn.pack(side=tk.LEFT, padx=(0, 4))
        self.stop_btn = ttk.Button(play_frame, text="Stop", command=self._on_stop)
        self.stop_btn.pack(side=tk.LEFT)

        # ---- Configuration frame ------------------------------------------
        cfg_frame = ttk.LabelFrame(main_frame, text="Configuration", padding=8)
        cfg_frame.pack(fill=tk.X, pady=(0, 8))

        self._add_cfg_row(cfg_frame, 0, "IP:",       "cfg_ip",       DEFAULT_IP)
        self._add_cfg_row(cfg_frame, 1, "MAC:",      "cfg_mac",      "DE:AD:BE:EF:BE:ED")
        self._add_cfg_row(cfg_frame, 2, "LED Width:", "cfg_led_width", "512")
        self._add_cfg_row(cfg_frame, 3, "Universes:", "cfg_universes", "0,3,6,9,12,15,18,21")
        self._add_cfg_row(cfg_frame, 4, "Outputs:",   "cfg_outputs",   "1,1,1,1,1,1,1,1")

        ttk.Button(cfg_frame, text="Apply All", command=self._on_config_apply)\
            .grid(row=5, column=0, columnspan=3, pady=(6, 0))

        # ---- Status frame -------------------------------------------------
        status_frame = ttk.LabelFrame(main_frame, text="Status", padding=8)
        status_frame.pack(fill=tk.X, pady=(0, 8))

        self.status_text = tk.Text(status_frame, height=4, wrap=tk.NONE, font=("Consolas", 9))
        self.status_text.pack(fill=tk.X, side=tk.LEFT, expand=True)
        status_scroll = ttk.Scrollbar(status_frame, orient=tk.VERTICAL, command=self.status_text.yview)
        status_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.status_text.config(yscrollcommand=status_scroll.set)

        btn_frame = ttk.Frame(status_frame)
        btn_frame.pack(fill=tk.X, pady=(4, 0))
        ttk.Button(btn_frame, text="Refresh", command=self._on_status).pack(side=tk.RIGHT)

        # ---- Log frame ----------------------------------------------------
        log_frame = ttk.LabelFrame(main_frame, text="Log", padding=8)
        log_frame.pack(fill=tk.BOTH, expand=True)

        self.log_text = scrolledtext.ScrolledText(
            log_frame, height=8, wrap=tk.WORD,
            font=("Consolas", 9), state=tk.DISABLED)
        self.log_text.pack(fill=tk.BOTH, expand=True)

    def _populate_interfaces(self):
        """Fill the adapter dropdown with detected interfaces."""
        ifaces = get_interfaces()
        self._interface_map = {}
        items = ["(auto)"]
        for ip, name in ifaces:
            label = "%s  (%s)" % (ip, name)
            self._interface_map[label] = ip
            items.append(label)
        self.iface_combo["values"] = items
        self.iface_var.set("(auto)")

    def _add_cfg_row(self, parent, row, label, attr, default):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky=tk.W, padx=(0, 8))
        var = tk.StringVar(value=default)
        setattr(self, attr + "_var", var)
        ttk.Entry(parent, textvariable=var, width=32).grid(row=row, column=1, sticky=tk.W, padx=(0, 12))

    def _set_conn_indicator(self, color):
        self.conn_indicator.delete("all")
        r = 6
        cx, cy = 7, 7
        self.conn_indicator.create_oval(cx - r, cy - r, cx + r, cy + r,
                                         fill=color, outline="black")

    # ---- COMMAND DISPATCH ------------------------------------------------

    def _send(self, cmd):
        if self.client and self.client.connected:
            self.send_queue.put(cmd)
        else:
            self._log("Cannot send, not connected")

    def _on_connect(self):
        host = self.ip_var.get().strip()
        if not host:
            messagebox.showerror("Error", "IP address is required")
            return
        self._start_client(host, DEFAULT_PORT)

    def _on_disconnect(self):
        if self.client:
            self.client.stop()
            self.client = None
        self.connected = False
        self._set_connected_ui(False)

    def _start_client(self, host, port):
        if self.client:
            self.client.stop()

        # Get the selected adapter IP (or None for auto)
        selected_label = self.iface_var.get()
        bind_ip = self._interface_map.get(selected_label)

        self.client = TcpClient(
            host, port,
            self.send_queue, self.recv_queue,
            self._log, self._on_status_update,
            bind_ip=bind_ip
        )
        self.client.start()
        self._set_connected_ui(True, connecting=True)

    def _on_status_update(self, connected, latency_ms):
        self.connected = connected
        self.root.after(0, self._update_connection_ui, connected, latency_ms)

    def _update_connection_ui(self, connected, latency_ms):
        self._set_connected_ui(connected)
        if connected:
            self.latency_var.set("%.1f ms" % latency_ms)
            self._set_conn_indicator("green")
        else:
            self.latency_var.set("-- ms")
            self._set_conn_indicator("red")

    def _set_connected_ui(self, connected, connecting=False):
        if connected:
            self.connect_btn.config(state=tk.DISABLED)
            self.disconnect_btn.config(state=tk.NORMAL)
            self._set_conn_indicator("green" if not connecting else "orange")
        else:
            self.connect_btn.config(state=tk.NORMAL)
            self.disconnect_btn.config(state=tk.DISABLED)
            self._set_conn_indicator("red")

    # ---- MODE ------------------------------------------------------------

    def _on_mode_change(self):
        mode = self.mode_var.get()
        cmd = "MODE:%s" % mode
        self._send(cmd)

    # ---- RECORDING -------------------------------------------------------

    def _on_rec_start(self):
        self._send("REC:START")
        self.rec_start_btn.config(state=tk.DISABLED)
        self.rec_stop_btn.config(state=tk.NORMAL)
        self.rec_status_var.set("Recording...")

    def _on_rec_stop(self):
        self._send("REC:STOP")
        self.rec_start_btn.config(state=tk.NORMAL)
        self.rec_stop_btn.config(state=tk.DISABLED)
        self.rec_status_var.set("Idle")

    # ---- PLAYBACK --------------------------------------------------------

    def _on_play_file(self):
        fn = self.play_file_var.get().strip()
        if not fn:
            messagebox.showerror("Error", "Enter a .BIN filename first")
            return
        self._send("PLAY:%s" % fn)

    def _on_play_sequence(self):
        self._send("PLAY:SEQUENCE")

    def _on_stop(self):
        self._send("STOP")
        self.rec_start_btn.config(state=tk.NORMAL)
        self.rec_stop_btn.config(state=tk.DISABLED)
        self.rec_status_var.set("Idle")

    # ---- CONFIG ----------------------------------------------------------

    def _on_config_apply(self):
        ip       = self.cfg_ip_var.get().strip()
        mac      = self.cfg_mac_var.get().strip()
        led_w    = self.cfg_led_width_var.get().strip()
        univ     = self.cfg_universes_var.get().strip()
        outputs  = self.cfg_outputs_var.get().strip()

        if not messagebox.askyesno("Apply Configuration",
                                     "This will save the new configuration to the SD card\n"
                                     "and reboot the Teensy.\n\n"
                                     "Continue?"):
            return

        self._send("CONFIG:ip=%s" % ip)
        # Wait a moment, then send the rest (they queue up)
        time.sleep(0.05)
        self._send("CONFIG:mac=%s" % mac)
        self._send("CONFIG:led_width=%s" % led_w)
        self._send("CONFIG:start_universe=%s" % univ)
        self._send("CONFIG:output_active=%s" % outputs)
        self._log("Configuration commands queued (Teensy will reboot)")

    # ---- STATUS ----------------------------------------------------------

    def _on_status(self):
        self._send("STATUS")

    # ---- QUEUE POLLING ---------------------------------------------------

    def _poll_queue(self):
        try:
            while True:
                resp = self.recv_queue.get_nowait()
                self._update_status(resp)
        except queue.Empty:
            pass
        self.root.after(100, self._poll_queue)

    def _update_status(self, resp):
        # Display raw status in the status text widget
        self.status_text.insert(tk.END, resp + "\n")
        self.status_text.see(tk.END)

    # ---- LOGGING ---------------------------------------------------------

    def _log(self, msg):
        ts = datetime.now().strftime("%H:%M:%S")
        self.log_text.config(state=tk.NORMAL)
        self.log_text.insert(tk.END, "[%s] %s\n" % (ts, msg))
        self.log_text.see(tk.END)
        self.log_text.config(state=tk.DISABLED)

    # ---- CLEANUP ---------------------------------------------------------

    def on_close(self):
        if self.client:
            self.client.stop()
        self.root.destroy()


# ============================================================================
# Entry point
# ============================================================================
def main():
    root = tk.Tk()
    app = UzomaBoxApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()


if __name__ == "__main__":
    main()