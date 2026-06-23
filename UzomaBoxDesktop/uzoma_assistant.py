#!/usr/bin/env python3
"""
UzomaBox Assistant
===================
Multi-device controller for UzomaBox Teensy 4.1 LED controllers.

Inspired by the Advatek PixLite Assistant workflow:
  - Discovers all UzomaBox controllers on the LAN via UDP broadcast
  - Shows them in a sortable table (Model, Nickname, IP, FW, Temp)
  - Double-click opens a per-device tabbed configuration window
  - Uses persistent TCP connections — no race conditions
  - No external dependencies — uses Python stdlib only

Protocol (TCP port 8888):
  All commands from the original uzoma_box_desktop.py are supported.
  Discovery uses UDP broadcast on port 7777.

UDP Discovery:
  Send (broadcast): UZOMA:SEARCH
  Receive (unicast): MODEL=<m>,NICK=<n>,IP=<ip>,FW=<fw>,TEMP=<t>
"""

import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox
import socket
import threading
import time
import queue
import subprocess
import re
import os
from datetime import datetime

# ============================================================================
# Configuration
# ============================================================================
DISCOVERY_PORT   = 7777
DISCOVERY_TIMEOUT = 2.0
TCP_PORT         = 8888

# ============================================================================
# UDP Discovery
# ============================================================================
def discover_controllers(bind_ip=None, timeout=DISCOVERY_TIMEOUT):
    """Send UDP broadcast, collect responses. Returns list of dicts."""
    devices = []
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(timeout)
    if bind_ip:
        sock.bind((bind_ip, 0))

    try:
        sock.sendto(b"UZOMA:SEARCH", ("255.255.255.255", DISCOVERY_PORT))
        while True:
            try:
                data, addr = sock.recvfrom(1024)
                line = data.decode("utf-8", errors="replace").strip()
                info = {"ip": addr[0]}
                for part in line.split(","):
                    if "=" in part:
                        k, v = part.split("=", 1)
                        info[k.lower()] = v
                devices.append(info)
            except socket.timeout:
                break
    except Exception:
        pass
    finally:
        sock.close()
    return devices


# ============================================================================
# Persistent TCP Client (single connection, thread-safe)
# ============================================================================
class TcpClientPersistent:
    """
    Persistent TCP connection to a single device.
    All commands go through this single connection using a lock,
    eliminating race conditions between STATUS polls and user commands.
    """

    def __init__(self, host, port=TCP_PORT, timeout=5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None
        self.lock = threading.Lock()

    def connect(self):
        if self.sock:
            return
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect((self.host, self.port))
        # Drain OK:connected banner
        try:
            self.sock.recv(1024)
        except socket.timeout:
            pass

    def send_and_recv(self, cmd):
        """
        Send a command and return all response lines.
        Thread-safe via lock.
        Uses a small recv timeout to drain the TCP socket completely,
        preventing stale data from bleeding across commands.
        """
        with self.lock:
            if not self.sock:
                self.connect()
            # Set a short timeout so we can detect "no more data"
            orig_timeout = self.sock.gettimeout()
            self.sock.settimeout(0.3)  # 300 ms drain timeout
            try:
                self.sock.sendall((cmd + "\n").encode("utf-8"))
            except:
                pass
            lines = []
            buf = b""
            try:
                while True:
                    try:
                        chunk = self.sock.recv(4096)
                        if not chunk:
                            break
                        buf += chunk
                    except socket.timeout:
                        # No more data within 300 ms — drain complete
                        break
                    # Process all complete lines from the buffer
                    while True:
                        idx = -1
                        sep_len = 0
                        for sep in (b"\r\n", b"\n", b"\r"):
                            i = buf.find(sep)
                            if i >= 0 and (idx < 0 or i < idx):
                                idx = i
                                sep_len = len(sep)
                        if idx < 0:
                            break
                        line = buf[:idx].decode("utf-8", errors="replace").strip()
                        buf = buf[idx + sep_len:]
                        if line:
                            lines.append(line)
            except Exception:
                pass
            finally:
                # Restore original timeout (or default 5.0s)
                self.sock.settimeout(orig_timeout if orig_timeout else 5.0)
            return lines

    def send_only(self, cmd):
        """Send a command without waiting for response (fire-and-forget)."""
        with self.lock:
            if not self.sock:
                self.connect()
            self.sock.sendall((cmd + "\n").encode("utf-8"))

    def close(self):
        with self.lock:
            if self.sock:
                try:
                    self.sock.close()
                except:
                    pass
                self.sock = None


# ============================================================================
# Network interface enumeration
# ============================================================================
def get_interfaces():
    """
    Return list of (ip, name) tuples for all non-loopback IPv4 interfaces.
    Uses only ipconfig on Windows — socket.getaddrinfo() with DNS can hang
    for 30+ seconds on systems with virtual adapters (Hyper-V, VPN, WSL).
    """
    interfaces = []
    try:
        output = subprocess.check_output(
            "ipconfig", shell=True, stderr=subprocess.DEVNULL, timeout=5
        ).decode("utf-8", errors="replace")
        lines = output.splitlines()
        current_adapter = None
        for line in lines:
            m = re.match(r'^(.+?)\s+adapter\s+.+?:', line, re.IGNORECASE)
            if m:
                current_adapter = m.group(1).strip()
                continue
            m = re.search(r'IPv4[.\s]*Address[.\s]*[:.]+[ \t]*([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)', line)
            if not m:
                m = re.search(r'IP[.\s]*Address[.\s]*[:.]+[ \t]*([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)', line)
            if m and current_adapter:
                ip = m.group(1)
                if not ip.startswith("127."):
                    if (ip, current_adapter) not in interfaces:
                        interfaces.append((ip, current_adapter))
                    current_adapter = None
    except Exception:
        pass
    seen = set()
    unique = []
    for ip, name in interfaces:
        if ip not in seen:
            seen.add(ip)
            unique.append((ip, name))
    if unique:
        return unique
    # Fallback: gethostbyname
    try:
        hostname = socket.gethostname()
        ip = socket.gethostbyname(hostname)
        if not ip.startswith("127."):
            unique.append((ip, hostname))
    except:
        pass
    return unique


# ============================================================================
# Per-Device Configuration Window
# ============================================================================
class DeviceConfigWindow:
    """Tabbed configuration window with persistent TCP + always-visible status bar."""

    def __init__(self, parent, device_info, log_callback):
        self.device = device_info
        self.log = log_callback
        self.ip = device_info["ip"]
        self._status_data = {}
        self._file_list = []
        self._progress_active = False

        # Create persistent TCP connection
        self._tcp = TcpClientPersistent(self.ip, timeout=3.0)
        try:
            self._tcp.connect()
        except:
            pass

        # Build window
        self.win = tk.Toplevel(parent)
        self.win.title("UzomaBox - %s [%s]" % (device_info.get("nick", self.ip), self.ip))
        self.win.geometry("680x640")
        self.win.resizable(True, True)

        # === Always-visible status bar (above the tabs) ===
        status_frame = ttk.Frame(self.win, relief=tk.SUNKEN, borderwidth=1)
        status_frame.pack(fill=tk.X, side=tk.TOP, padx=4, pady=(4, 0))

        # Connection indicator (green/red circle)
        self._conn_indicator = tk.Canvas(status_frame, width=14, height=14, highlightthickness=0)
        self._conn_indicator.pack(side=tk.LEFT, padx=(4, 6))
        self._conn_indicator.create_oval(1, 1, 13, 13, fill="red", outline="black")

        self._status_labels = {}
        fields = [("mode", "Mode:"), ("playing", "Playing:"), ("recording", "Rec:"),
                  ("file", "File:"), ("speed", "Speed:"), ("record_time", "Time:")]
        for key, label in fields:
            f = ttk.Frame(status_frame)
            f.pack(side=tk.LEFT, padx=(6, 2))
            ttk.Label(f, text=label, font=("Consolas", 8, "bold")).pack(side=tk.LEFT)
            var = tk.StringVar(value="--")
            self._status_labels[key] = var
            ttk.Label(f, textvariable=var, font=("Consolas", 8)).pack(side=tk.LEFT)

        # Notebook (tabs)
        self.notebook = ttk.Notebook(self.win, padding=4)
        self.notebook.pack(fill=tk.BOTH, expand=True)

        self._build_network_tab()
        self._build_led_tab()
        self._build_control_tab()
        self._build_playback_tab()
        self._build_misc_tab()

        # Bottom status bar
        self.status_var = tk.StringVar(value="Ready")
        ttk.Label(self.win, textvariable=self.status_var, relief=tk.SUNKEN, anchor=tk.W).pack(
            fill=tk.X, side=tk.BOTTOM, padx=4, pady=(0, 4))

        # Start periodic STATUS polling on the persistent connection
        self.after_id = None
        self._poll_status()

    # ---- Network Tab (Tab 1) -----------------------------------------------

    def _build_network_tab(self):
        frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(frame, text="Network")
        ttk.Label(frame, text="Nickname:").grid(row=0, column=0, sticky=tk.W, padx=(0,8), pady=4)
        self.nick_var = tk.StringVar(value=self.device.get("nick", ""))
        ttk.Entry(frame, textvariable=self.nick_var, width=30).grid(row=0, column=1, sticky=tk.W, pady=4)
        ttk.Button(frame, text="Save", command=self._save_nickname).grid(row=0, column=2, padx=(8,0), pady=4)

        ttk.Label(frame, text="IP Address:").grid(row=1, column=0, sticky=tk.W, padx=(0,8), pady=4)
        self.ip_var = tk.StringVar(value=self.ip)
        ttk.Entry(frame, textvariable=self.ip_var, width=30).grid(row=1, column=1, sticky=tk.W, pady=4)
        ttk.Button(frame, text="Apply & Reboot", command=self._save_ip).grid(row=1, column=2, padx=(8,0), pady=4)

        ttk.Label(frame, text="MAC Address:").grid(row=2, column=0, sticky=tk.W, padx=(0,8), pady=4)
        self.mac_var = tk.StringVar(value="--")
        ttk.Label(frame, textvariable=self.mac_var, width=30, relief=tk.SUNKEN, anchor=tk.W).grid(
            row=2, column=1, sticky=tk.W, pady=4)

        ttk.Separator(frame, orient=tk.HORIZONTAL).grid(row=3, column=0, columnspan=3, sticky=tk.EW, pady=8)

        ttk.Label(frame, text="Model:").grid(row=4, column=0, sticky=tk.W, padx=(0,8), pady=2)
        self.model_var = tk.StringVar(value=self.device.get("model", "--"))
        ttk.Label(frame, textvariable=self.model_var).grid(row=4, column=1, sticky=tk.W, pady=2)

        ttk.Label(frame, text="Firmware:").grid(row=5, column=0, sticky=tk.W, padx=(0,8), pady=2)
        self.fw_var = tk.StringVar(value=self.device.get("fw", "--"))
        ttk.Label(frame, textvariable=self.fw_var).grid(row=5, column=1, sticky=tk.W, pady=2)

        ttk.Label(frame, text="Temperature:").grid(row=6, column=0, sticky=tk.W, padx=(0,8), pady=2)
        self.temp_var = tk.StringVar(value=self.device.get("temp", "--") + "°C")
        ttk.Label(frame, textvariable=self.temp_var).grid(row=6, column=1, sticky=tk.W, pady=2)

    # ---- LED Tab (Tab 2) ---------------------------------------------------

    def _build_led_tab(self):
        frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(frame, text="LEDs")

        # LEDs per strip + Color order at top
        top_frame = ttk.Frame(frame)
        top_frame.pack(fill=tk.X, pady=(0, 8))
        ttk.Label(top_frame, text="LEDs per Strip:").pack(side=tk.LEFT, padx=(0, 8))
        self.led_width_var = tk.StringVar(value="512")
        ttk.Entry(top_frame, textvariable=self.led_width_var, width=8).pack(side=tk.LEFT, padx=(0, 16))
        ttk.Label(top_frame, text="Color Order:").pack(side=tk.LEFT, padx=(0, 8))
        self.color_order_var = tk.StringVar(value="rgb")
        ttk.Combobox(top_frame, textvariable=self.color_order_var,
                      values=["rgb", "rbg", "grb", "gbr", "brg", "bgr"],
                      width=6, state="readonly").pack(side=tk.LEFT)

        # Per-output table header
        columns_frame = ttk.Frame(frame)
        columns_frame.pack(fill=tk.X, pady=(0, 2))
        # Column widths: Out=40, Active=50, StartUniv=80, StartCh=70, EndUniv=80, EndCh=70
        ttk.Label(columns_frame, text="Out", font=("TkDefaultFont", 8, "bold"),
                  width=5).pack(side=tk.LEFT, padx=(2, 0))
        ttk.Label(columns_frame, text="Active", font=("TkDefaultFont", 8, "bold"),
                  width=6).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Label(columns_frame, text="Start Univ", font=("TkDefaultFont", 8, "bold"),
                  width=10).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Label(columns_frame, text="Start Ch", font=("TkDefaultFont", 8, "bold"),
                  width=8).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Label(columns_frame, text="End Univ", font=("TkDefaultFont", 8, "bold"),
                  width=10).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Label(columns_frame, text="End Ch", font=("TkDefaultFont", 8, "bold"),
                  width=8).pack(side=tk.LEFT)

        # Separator
        ttk.Separator(frame, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=(0, 4))

        # 8 output rows
        self.output_active_vars = []
        self.start_univ_vars = []
        self.end_univ_vars = []
        self.end_ch_vars = []

        for i in range(8):
            row_frame = ttk.Frame(frame)
            row_frame.pack(fill=tk.X, pady=1)

            # Out number
            ttk.Label(row_frame, text="%d" % (i + 1), width=5).pack(side=tk.LEFT, padx=(2, 0))

            # Active checkbox
            active_var = tk.BooleanVar(value=True)
            self.output_active_vars.append(active_var)
            ttk.Checkbutton(row_frame, variable=active_var, width=4).pack(side=tk.LEFT, padx=(0, 4))

            # Start Universe (dropdown)
            start_var = tk.StringVar(value=str(i * 3))
            self.start_univ_vars.append(start_var)
            start_combo = ttk.Combobox(row_frame, textvariable=start_var,
                                        values=[str(x) for x in range(0, 256)],
                                        width=8, state="readonly")
            start_combo.pack(side=tk.LEFT, padx=(0, 4))
            start_combo.bind("<<ComboboxSelected>>", lambda e, idx=i: self._update_univ_range(idx))

            # Start Channel (always 1, read-only)
            ttk.Label(row_frame, text="1", width=8, anchor=tk.CENTER,
                      font=("Consolas", 9)).pack(side=tk.LEFT, padx=(0, 4))

            # End Universe (computed, read-only)
            end_univ_var = tk.StringVar(value="--")
            self.end_univ_vars.append(end_univ_var)
            ttk.Label(row_frame, textvariable=end_univ_var, width=10, anchor=tk.CENTER,
                      font=("Consolas", 9)).pack(side=tk.LEFT, padx=(0, 4))

            # End Channel (computed, read-only)
            end_ch_var = tk.StringVar(value="--")
            self.end_ch_vars.append(end_ch_var)
            ttk.Label(row_frame, textvariable=end_ch_var, width=8, anchor=tk.CENTER,
                      font=("Consolas", 9)).pack(side=tk.LEFT)

        # Also recompute when LEDs per strip changes
        self.led_width_var.trace_add("write", lambda *a: self._update_all_univ_ranges())

        # Initial computation
        self._update_all_univ_ranges()

        # Apply button
        ttk.Button(frame, text="Apply LED Settings (color live, rest reboots)",
                   command=self._save_led).pack(pady=12)

    def _compute_univ_range(self, led_width, start_univ):
        """Compute (end_univ, end_ch) for given LEDs and start universe."""
        try:
            pixels = int(led_width)
            start = int(start_univ)
        except (ValueError, TypeError):
            return ("--", "--")
        total_ch = pixels * 3
        end_ch = ((total_ch - 1) % 512) + 1
        end_univ = start + (total_ch - 1) // 512
        return (str(end_univ), str(end_ch))

    def _update_univ_range(self, idx):
        """Recompute end_univ and end_ch for output idx."""
        led_w = self.led_width_var.get()
        start_u = self.start_univ_vars[idx].get()
        end_u, end_ch = self._compute_univ_range(led_w, start_u)
        self.end_univ_vars[idx].set(end_u)
        self.end_ch_vars[idx].set(end_ch)

    def _update_all_univ_ranges(self):
        """Recompute end_univ and end_ch for all 8 outputs."""
        for i in range(8):
            self._update_univ_range(i)

    # ---- Control Tab (Tab 3) -----------------------------------------------

    def _build_control_tab(self):
        frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(frame, text="Control")

        ttk.Label(frame, text="Mode:").grid(row=0, column=0, sticky=tk.W, padx=(0,8), pady=4)
        self.mode_var = tk.StringVar(value="artnet")
        mode_frame = ttk.Frame(frame)
        mode_frame.grid(row=0, column=1, sticky=tk.W, pady=4)
        for mode in [("ArtNet", "artnet"), ("Playback", "playback"), ("Record", "record"), ("Test", "test")]:
            ttk.Radiobutton(mode_frame, text=mode[0], variable=self.mode_var,
                             value=mode[1], command=self._change_mode).pack(side=tk.LEFT, padx=(0,12))

        ttk.Label(frame, text="Record FPS:").grid(row=1, column=0, sticky=tk.W, padx=(0,8), pady=4)
        self.record_fps_var = tk.StringVar(value="30")
        ttk.Spinbox(frame, from_=5, to=60, textvariable=self.record_fps_var, width=6).grid(row=1, column=1, sticky=tk.W, pady=4)

        rec_frame = ttk.LabelFrame(frame, text="Recording", padding=6)
        rec_frame.grid(row=2, column=0, columnspan=2, sticky=tk.EW, pady=8)
        self.rec_start_btn = ttk.Button(rec_frame, text="▶ Start", command=self._rec_start)
        self.rec_start_btn.pack(side=tk.LEFT, padx=(0,6))
        self.rec_stop_btn = ttk.Button(rec_frame, text="■ Stop", command=self._rec_stop, state=tk.DISABLED)
        self.rec_stop_btn.pack(side=tk.LEFT, padx=(0,6))
        self.rec_status_var = tk.StringVar(value="Idle")
        ttk.Label(rec_frame, textvariable=self.rec_status_var).pack(side=tk.LEFT)
        self.rec_timer_var = tk.StringVar(value="")
        ttk.Label(rec_frame, textvariable=self.rec_timer_var, width=10).pack(side=tk.LEFT)

        ttk.Button(frame, text="Apply FPS (reboot)", command=self._save_record_fps).grid(
            row=3, column=0, columnspan=2, pady=4)

    # ---- Playback Tab (Tab 4) ----------------------------------------------

    def _build_playback_tab(self):
        frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(frame, text="Playback")

        file_frame = ttk.Frame(frame)
        file_frame.pack(fill=tk.X, pady=(0,6))
        ttk.Label(file_frame, text="File:").pack(side=tk.LEFT, padx=(0,4))
        self.play_file_var = tk.StringVar()
        ttk.Entry(file_frame, textvariable=self.play_file_var, width=22).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(file_frame, text="▶ Play", command=self._play_file, width=8).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(file_frame, text="■ Stop", command=self._stop, width=8).pack(side=tk.LEFT)

        list_frame = ttk.Frame(frame)
        list_frame.pack(fill=tk.BOTH, expand=True, pady=(0,6))
        self.file_listbox = tk.Listbox(list_frame, height=5, font=("Consolas", 9))
        file_scroll = ttk.Scrollbar(list_frame, orient=tk.VERTICAL, command=self.file_listbox.yview)
        self.file_listbox.config(yscrollcommand=file_scroll.set)
        self.file_listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        file_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.file_listbox.bind("<<ListboxSelect>>", self._on_file_select)

        btn_frame = ttk.Frame(frame)
        btn_frame.pack(fill=tk.X, pady=(0,6))
        ttk.Button(btn_frame, text="Refresh List", command=self._refresh_list).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(btn_frame, text="Play All", command=self._play_all).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(btn_frame, text="Delete Selected", command=self._delete_file).pack(side=tk.LEFT)

        speed_frame = ttk.Frame(frame)
        speed_frame.pack(fill=tk.X)
        ttk.Label(speed_frame, text="Speed:").pack(side=tk.LEFT, padx=(0,4))
        self.speed_var = tk.DoubleVar(value=1.0)
        speed_scale = ttk.Scale(speed_frame, from_=0.05, to=5.0, orient=tk.HORIZONTAL,
                                 variable=self.speed_var, command=self._on_speed_change, length=150)
        speed_scale.pack(side=tk.LEFT, padx=(0,4))
        self.speed_label_var = tk.StringVar(value="1.00x")
        ttk.Label(speed_frame, textvariable=self.speed_label_var, width=8).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(speed_frame, text="Set", command=self._set_speed, width=6).pack(side=tk.LEFT)

        progress_frame = ttk.Frame(frame)
        progress_frame.pack(fill=tk.X, pady=(6,0))
        ttk.Label(progress_frame, text="Progress:").pack(side=tk.LEFT, padx=(0,4))
        self.progress_var = tk.DoubleVar(value=0.0)
        self.progress_bar = ttk.Progressbar(progress_frame, variable=self.progress_var, length=200, mode="determinate")
        self.progress_bar.pack(side=tk.LEFT, padx=(0,4))
        self.progress_label_var = tk.StringVar(value="-- / --")
        ttk.Label(progress_frame, textvariable=self.progress_label_var).pack(side=tk.LEFT)

    # ---- Status Tab (Tab 5) ------------------------------------------------

    def _build_misc_tab(self):
        frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(frame, text="Status")
        ttk.Label(frame, text="Raw Device Status:").pack(anchor=tk.W)
        self.status_text = tk.Text(frame, height=12, wrap=tk.NONE, font=("Consolas", 9))
        status_scroll = ttk.Scrollbar(frame, orient=tk.VERTICAL, command=self.status_text.yview)
        self.status_text.config(yscrollcommand=status_scroll.set)
        self.status_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        status_scroll.pack(side=tk.RIGHT, fill=tk.Y)

        btn_frame = ttk.Frame(frame)
        btn_frame.pack(fill=tk.X, pady=(6,0))
        ttk.Button(btn_frame, text="Refresh Status", command=self._request_status).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(btn_frame, text="Identify (Flash LED)", command=self._identify).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(btn_frame, text="Close", command=self.win.destroy).pack(side=tk.RIGHT)

    # ---- Persistent TCP Helpers --------------------------------------------

    def _cmd(self, cmd):
        """Send a command and return response lines."""
        return self._tcp.send_and_recv(cmd)

    def _cmd_send(self, cmd):
        """Send a command (fire-and-forget)."""
        self._tcp.send_only(cmd)

    # ---- Network Tab Actions -----------------------------------------------

    def _save_nickname(self):
        nick = self.nick_var.get().strip()
        if nick:
            self._cmd_send("CONFIG:nickname=%s" % nick)
            self.status_var.set("Nickname saved (live)")
            self.log("Set nickname on %s: %s" % (self.ip, nick))

    def _save_ip(self):
        new_ip = self.ip_var.get().strip()
        if not new_ip:
            return
        if not messagebox.askyesno("Change IP", "Changing IP to %s\nDevice will reboot.\nContinue?" % new_ip):
            return
        self._cmd_send("CONFIG:ip=%s" % new_ip)
        self.log("Changed IP on %s to %s (rebooting)" % (self.ip, new_ip))
        self.win.destroy()

    # ---- LED Tab Actions ---------------------------------------------------

    def _save_led(self):
        color = self.color_order_var.get().strip()
        led_w = self.led_width_var.get().strip()
        univ = ",".join(v.get() for v in self.start_univ_vars)
        outputs = ",".join("1" if v.get() else "0" for v in self.output_active_vars)

        # Color order applies live first (no reboot needed)
        self._cmd_send("CONFIG:color_order=%s" % color)
        time.sleep(0.1)

        # Send start_universe FIRST among reboot-required configs so it gets
        # saved to CONFIG.TXT before the first reboot-triggering command.
        # led_width and output_active cause reboots too, so they go after.
        self._cmd_send("CONFIG:start_universe=%s" % univ)
        time.sleep(0.05)
        self._cmd_send("CONFIG:led_width=%s" % led_w)
        self._cmd_send("CONFIG:output_active=%s" % outputs)
        self.log("LED settings updated on %s (rebooting)" % self.ip)
        self.win.destroy()

    # ---- Control Tab Actions -----------------------------------------------

    def _change_mode(self):
        mode = self.mode_var.get()
        self._cmd_send("MODE:%s" % mode)
        self.status_var.set("Switching to %s mode" % mode)
        self.log("Mode set to %s on %s" % (mode, self.ip))

    def _save_record_fps(self):
        fps = self.record_fps_var.get().strip()
        self._cmd_send("CONFIG:record_fps=%s" % fps)
        self.log("Record FPS updated on %s (rebooting)" % self.ip)
        self.win.destroy()

    def _rec_start(self):
        self._cmd_send("REC:START")
        self.rec_start_btn.config(state=tk.DISABLED)
        self.rec_stop_btn.config(state=tk.NORMAL)
        self.rec_status_var.set("Recording...")
        self.log("Recording started on %s" % self.ip)

    def _rec_stop(self):
        self._cmd_send("REC:STOP")
        self.rec_start_btn.config(state=tk.NORMAL)
        self.rec_stop_btn.config(state=tk.DISABLED)
        self.rec_status_var.set("Idle")
        self.log("Recording stopped on %s" % self.ip)

    # ---- Playback Tab Actions ----------------------------------------------

    def _play_file(self):
        fn = self.play_file_var.get().strip()
        if not fn:
            messagebox.showerror("Error", "Enter a filename first")
            return
        self._cmd_send("PLAY:%s" % fn)
        self.log("Playing %s on %s" % (fn, self.ip))

    def _play_all(self):
        self._cmd_send("PLAY:SEQUENCE")
        self.log("Play all on %s" % self.ip)

    def _stop(self):
        self._cmd_send("STOP")
        self.rec_start_btn.config(state=tk.NORMAL)
        self.rec_stop_btn.config(state=tk.DISABLED)
        self.rec_status_var.set("Idle")
        self.log("Stop on %s" % self.ip)

    def _on_speed_change(self, *args):
        self.speed_label_var.set("%.2fx" % self.speed_var.get())

    def _set_speed(self):
        speed = self.speed_var.get()
        speed = max(0.05, min(5.0, speed))
        self._cmd_send("SPEED:%.2f" % speed)
        self.log("Speed %.2fx on %s" % (speed, self.ip))

    def _refresh_list(self):
        # Use the persistent TCP connection (locked, single-client firmware)
        lines = self._cmd("LIST")
        # Filter: exclude OK/END markers, exclude STATUS key=value lines,
        # and only keep lines that look like filenames (contain a dot or are
        # reasonable 8.3-style SD card filenames).
        files = []
        for l in lines:
            if l.startswith("OK:") or l.startswith("END:"):
                continue
            # Skip STATUS key=value lines (e.g. "mode=artnet", "ip=192...")
            if "=" in l:
                continue
            # Skip empty lines
            if not l.strip():
                continue
            files.append(l)
        self._file_list = files
        self.file_listbox.delete(0, tk.END)
        for fn in sorted(files):
            self.file_listbox.insert(tk.END, fn)

    def _on_file_select(self, event):
        sel = self.file_listbox.curselection()
        if sel and sel[0] < len(self._file_list):
            self.play_file_var.set(self._file_list[sel[0]])

    def _delete_file(self):
        sel = self.file_listbox.curselection()
        if not sel or sel[0] >= len(self._file_list):
            messagebox.showerror("Error", "Select a file first")
            return
        fn = self._file_list[sel[0]]
        if not messagebox.askyesno("Delete", "Delete %s?" % fn):
            return
        self._cmd_send("DELETE:%s" % fn)
        self.log("Deleted %s from %s" % (fn, self.ip))
        time.sleep(0.3)
        self._refresh_list()

    # ---- Misc / Status Tab Actions -----------------------------------------

    def _identify(self):
        self._cmd_send("IDENTIFY")
        self.status_var.set("Identify sent (LED flashes)")
        self.log("Identify on %s" % self.ip)

    def _request_status(self):
        lines = self._cmd("STATUS")
        self.status_text.delete("1.0", tk.END)
        for line in lines:
            self.status_text.insert(tk.END, line + "\n")
            self._parse_status_line(line)
        self.status_var.set("Status updated")

    def _parse_status_line(self, line):
        """Parse key=value from STATUS and update both tab fields and always-visible bar."""
        if "=" not in line:
            return
        k, v = line.split("=", 1)
        k = k.strip()
        v = v.strip()
        self._status_data[k] = v

        # Update tab fields
        if k == "mac":
            self.mac_var.set(v)
        elif k == "color_order":
            self.color_order_var.set(v)
        elif k == "led_width":
            self.led_width_var.set(v)
        elif k == "start_universe":
            # Parse comma-separated universes into per-output dropdowns
            parts = v.split(",")
            for i, p in enumerate(parts):
                if i < 8:
                    self.start_univ_vars[i].set(p.strip())
            self._update_all_univ_ranges()
        elif k == "record_fps":
            self.record_fps_var.set(v)
        elif k == "mode":
            self.mode_var.set(v)
        elif k == "recording":
            if v.lower() == "yes":
                self.rec_status_var.set("Recording...")
            else:
                self.rec_status_var.set("Idle")
        elif k == "record_time":
            try:
                secs = int(v)
                self.rec_timer_var.set("%02d:%02d" % (secs // 60, secs % 60))
            except:
                pass
        elif k == "playback_speed":
            try:
                self.speed_var.set(float(v))
                self.speed_label_var.set("%.2fx" % float(v))
            except:
                pass
        elif k == "file_pos" or k == "file_total":
            self._update_progress()

        # Update always-visible status bar
        bar_map = {
            "mode": "mode", "playing": "playing", "recording": "recording",
            "file": "file", "playback_speed": "speed", "record_time": "record_time"
        }
        if k in bar_map:
            bar_key = bar_map[k]
            if k == "playback_speed":
                self._status_labels["speed"].set(v + "x")
            elif k == "record_time":
                try:
                    secs = int(v)
                    self._status_labels["record_time"].set("%02d:%02d" % (secs // 60, secs % 60))
                except:
                    pass
            elif k == "recording":
                self._status_labels["recording"].set(v.capitalize())
            else:
                self._status_labels[bar_key].set(v)

    def _update_progress(self):
        try:
            pos = int(self._status_data.get("file_pos", "0"))
            total = int(self._status_data.get("file_total", "0"))
            if total > 0:
                pct = min(100.0, (pos / total) * 100.0)
                self.progress_var.set(pct)
                self.progress_label_var.set("%d / %d KB" % (pos // 1024, max(1, total // 1024)))
            else:
                self.progress_var.set(0.0)
        except:
            pass

    # ---- Periodic Polling (uses the persistent connection) -----------------

    def _poll_status(self):
        """Poll STATUS every 5 seconds using the persistent TCP connection."""
        try:
            if self.win.winfo_exists():
                try:
                    lines = self._cmd("STATUS")
                    # If we got lines, connection is good — green indicator
                    if lines:
                        self._set_conn_green()
                    for line in lines:
                        self._parse_status_line(line)
                except Exception:
                    self._set_conn_red()
                self.after_id = self.win.after(100, self._poll_status)
        except tk.TclError:
            pass

    def _set_conn_green(self):
        """Set connection indicator to green."""
        self._conn_indicator.delete("all")
        self._conn_indicator.create_oval(1, 1, 13, 13, fill="green", outline="black")

    def _set_conn_red(self):
        """Set connection indicator to red."""
        self._conn_indicator.delete("all")
        self._conn_indicator.create_oval(1, 1, 13, 13, fill="red", outline="black")

    def on_close(self):
        if self.after_id:
            try:
                self.win.after_cancel(self.after_id)
            except:
                pass
        self._tcp.close()
        self.win.destroy()


# ============================================================================
# Main Assistant Application
# ============================================================================
class UzomaBoxAssistant:
    def __init__(self, root):
        self.root = root
        root.title("UzomaBox Assistant")
        root.geometry("800x500")
        root.minsize(600, 300)

        self._interface_map = {}
        self._devices = []
        self._device_windows = {}
        self._searching = False

        # Load logo
        self._logo = None
        script_dir = os.path.dirname(os.path.abspath(__file__))
        logo_path = os.path.join(script_dir, "..", "black.png")
        if os.path.exists(logo_path):
            try:
                # subsample(8,8) reduces from 1920x562 to 240x70 for the header
                self._logo = tk.PhotoImage(file=logo_path).subsample(8, 8)
            except:
                pass

        self._build_ui()
        # Run interface detection in background thread — ipconfig can take seconds
        threading.Thread(target=self._thread_populate_interfaces, daemon=True).start()

    # ---- UI BUILDING -------------------------------------------------------

    def _build_ui(self):
        main_frame = ttk.Frame(self.root, padding=8)
        main_frame.pack(fill=tk.BOTH, expand=True)

        # Header with logo
        header = ttk.Frame(main_frame)
        header.pack(fill=tk.X, pady=(0, 4))

        if self._logo:
            logo_label = ttk.Label(header, image=self._logo)
            logo_label.pack(side=tk.LEFT, padx=(0, 10))

        title_frame = ttk.Frame(header)
        title_frame.pack(side=tk.LEFT)
        ttk.Label(title_frame, text="UzomaBox Assistant",
                  font=("TkDefaultFont", 14, "bold")).pack(anchor=tk.W)
        ttk.Label(title_frame, text="Multi-Device LED Controller Manager",
                  font=("TkDefaultFont", 9)).pack(anchor=tk.W)

        # Toolbar
        toolbar = ttk.Frame(main_frame)
        toolbar.pack(fill=tk.X, pady=(4, 6))

        ttk.Label(toolbar, text="Adapter:").pack(side=tk.LEFT, padx=(0, 4))
        self.iface_var = tk.StringVar()
        self.iface_combo = ttk.Combobox(toolbar, textvariable=self.iface_var,
                                         width=28, state="readonly")
        self.iface_combo.pack(side=tk.LEFT, padx=(0, 8))
        self.iface_combo["values"] = ["(auto)"]
        self.iface_var.set("(auto)")

        ttk.Button(toolbar, text="Refresh Adapters",
                   command=self._populate_interfaces).pack(side=tk.LEFT, padx=(0, 8))

        self.search_btn = ttk.Button(toolbar, text="🔍 Search for Controllers",
                                      command=self._search_devices)
        self.search_btn.pack(side=tk.LEFT, padx=(0, 8))

        self.status_label = ttk.Label(toolbar, text="Ready")
        self.status_label.pack(side=tk.RIGHT, padx=(4, 0))

        # Device table
        columns = ("model", "nickname", "ip", "fw", "temp")
        self.tree = ttk.Treeview(main_frame, columns=columns, show="headings",
                                  selectmode="browse", height=12)
        self.tree.heading("model", text="Model", command=lambda: self._sort_by("model"))
        self.tree.heading("nickname", text="Nickname", command=lambda: self._sort_by("nickname"))
        self.tree.heading("ip", text="IP Address", command=lambda: self._sort_by("ip"))
        self.tree.heading("fw", text="Firmware", command=lambda: self._sort_by("fw"))
        self.tree.heading("temp", text="Temp °C", command=lambda: self._sort_by("temp"))
        self.tree.column("model", width=120)
        self.tree.column("nickname", width=150)
        self.tree.column("ip", width=140)
        self.tree.column("fw", width=80)
        self.tree.column("temp", width=80)

        tree_frame = ttk.Frame(main_frame)
        tree_frame.pack(fill=tk.BOTH, expand=True)
        v_scroll = ttk.Scrollbar(tree_frame, orient=tk.VERTICAL, command=self.tree.yview)
        h_scroll = ttk.Scrollbar(tree_frame, orient=tk.HORIZONTAL, command=self.tree.xview)
        self.tree.config(yscrollcommand=v_scroll.set, xscrollcommand=h_scroll.set)
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        v_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        h_scroll.pack(side=tk.BOTTOM, fill=tk.X)

        self.tree.bind("<Double-1>", self._on_device_double_click)

        # Bottom status bar
        self.bottom_var = tk.StringVar(value="No devices found")
        ttk.Label(main_frame, textvariable=self.bottom_var, relief=tk.SUNKEN, anchor=tk.W).pack(
            fill=tk.X, side=tk.BOTTOM, pady=(4, 0))

        # Context menu
        self.context_menu = tk.Menu(self.root, tearoff=0)
        self.context_menu.add_command(label="Open Configuration", command=self._context_open)
        self.context_menu.add_command(label="Identify (Flash LED)", command=self._context_identify)
        self.context_menu.add_separator()
        self.context_menu.add_command(label="Delete from List", command=self._context_delete)
        self.tree.bind("<Button-3>", self._on_context_menu)

    # ---- Interface Population -----------------------------------------------

    def _thread_populate_interfaces(self):
        """Run get_interfaces() in a background thread, then update UI on main thread."""
        ifaces = get_interfaces()
        self.root.after(0, lambda: self._update_interfaces(ifaces))

    def _update_interfaces(self, ifaces):
        """Update the adapter dropdown (must be called on main thread)."""
        self._interface_map = {}
        items = ["(auto)"]
        for ip, name in ifaces:
            label = "%s  (%s)" % (ip, name)
            self._interface_map[label] = ip
            items.append(label)
        self.iface_combo["values"] = items
        if not items:
            items = ["(auto)"]
        self.iface_var.set(items[0])

    def _populate_interfaces(self):
        """Refresh Adapters button — runs in background to keep UI responsive."""
        self.bottom_var.set("Searching adapters...")
        threading.Thread(target=self._thread_populate_interfaces, daemon=True).start()

    # ---- Device Discovery ---------------------------------------------------

    def _search_devices(self):
        if self._searching:
            return
        self._searching = True
        self.search_btn.config(state=tk.DISABLED)
        self.status_label.config(text="Searching...")
        self.bottom_var.set("Scanning network for controllers...")

        selected_label = self.iface_var.get()
        bind_ip = self._interface_map.get(selected_label)

        def do_search():
            devices = discover_controllers(bind_ip=bind_ip)
            self.root.after(0, self._on_discovery_result, devices)

        threading.Thread(target=do_search, daemon=True).start()

    def _on_discovery_result(self, devices):
        self._devices = devices
        self._searching = False
        self.search_btn.config(state=tk.NORMAL)
        for item in self.tree.get_children():
            self.tree.delete(item)
        for dev in devices:
            self.tree.insert("", tk.END, values=(
                dev.get("model", "--"),
                dev.get("nick", "--"),
                dev.get("ip", "--"),
                dev.get("fw", "--"),
                dev.get("temp", "--")
            ))
        n = len(devices)
        self.status_label.config(text="Found %d controller(s)" % n)
        self.bottom_var.set("%d device(s) discovered" % n)

    # ---- Tree Sort ---------------------------------------------------------

    def _sort_by(self, col):
        items = [(self.tree.set(child, col), child) for child in self.tree.get_children("")]
        items.sort()
        for idx, (val, child) in enumerate(items):
            self.tree.move(child, "", idx)

    # ---- Device Actions ----------------------------------------------------

    def _get_selected_device(self):
        sel = self.tree.selection()
        if not sel:
            return None
        values = self.tree.item(sel[0], "values")
        ip = values[2] if len(values) > 2 else None
        if not ip:
            return None
        for dev in self._devices:
            if dev.get("ip") == ip:
                return dev
        return {"model": values[0], "nick": values[1], "ip": ip,
                "fw": values[3], "temp": values[4]}

    def _on_device_double_click(self, event):
        self._context_open()

    def _context_open(self):
        dev = self._get_selected_device()
        if not dev:
            return
        ip = dev["ip"]
        if ip in self._device_windows:
            try:
                self._device_windows[ip].win.lift()
                return
            except:
                del self._device_windows[ip]
        self._device_windows[ip] = DeviceConfigWindow(self.root, dev, self._log)
        self._log("Opened config for %s [%s]" % (dev.get("nick", ip), ip))

    def _context_identify(self):
        dev = self._get_selected_device()
        if not dev:
            return
        ip = dev["ip"]
        try:
            c = TcpClientPersistent(ip, timeout=2.0)
            c.send_only("IDENTIFY")
            c.close()
            self._log("Identify sent to %s [%s]" % (dev.get("nick", ip), ip))
        except Exception as e:
            self._log("Identify error on %s: %s" % (ip, e))

    def _context_delete(self):
        sel = self.tree.selection()
        if sel:
            self.tree.delete(sel[0])

    def _on_context_menu(self, event):
        item = self.tree.identify_row(event.y)
        if item:
            self.tree.selection_set(item)
            self.context_menu.post(event.x_root, event.y_root)

    # ---- Logging -----------------------------------------------------------

    def _log(self, msg):
        ts = datetime.now().strftime("%H:%M:%S")
        print("[%s] %s" % (ts, msg))


# ============================================================================
# Entry point
# ============================================================================
def main():
    root = tk.Tk()
    app = UzomaBoxAssistant(root)
    root.mainloop()


if __name__ == "__main__":
    main()