#!/usr/bin/env python3
"""
UzomaBox Assistant
===================
Multi-device controller for UzomaBox Teensy 4.1 LED controllers.

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
# Persistent TCP Client
# ============================================================================
class TcpClientPersistent:
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
        try:
            self.sock.recv(1024)
        except socket.timeout:
            pass

    def send_and_recv(self, cmd):
        with self.lock:
            if not self.sock:
                self.connect()
            orig_timeout = self.sock.gettimeout()
            self.sock.settimeout(0.3)
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
                        break
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
                self.sock.settimeout(orig_timeout if orig_timeout else 5.0)
            return lines

    def send_only(self, cmd):
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
    try:
        hostname = socket.gethostname()
        ip = socket.gethostbyname(hostname)
        if not ip.startswith("127."):
            unique.append((ip, hostname))
    except:
        pass
    return unique

# ============================================================================
# Per-Device Configuration Window (Tabbed)
# ============================================================================
class DeviceConfigWindow:
    def __init__(self, parent, device_info, log_callback):
        self.device = device_info
        self.log = log_callback
        self.ip = device_info["ip"]
        self._status_data = {}
        self._file_list = []
        self._progress_active = False
        self._num_outputs = 8  # default until we query the firmware
        self._start_univ_dirty = []
        self._color_order_dirty = False
        self._output_active_dirty = False
        self._breath_active = False
        self._breath_val = 0.0
        self._breath_after_id = None

        self._tcp = TcpClientPersistent(self.ip, timeout=3.0)
        try:
            self._tcp.connect()
            # Query the firmware for its actual output count
            lines = self._tcp.send_and_recv("NUM_OUTPUTS?")
            for line in lines:
                if line.startswith("NUM_OUTPUTS="):
                    try:
                        self._num_outputs = int(line.split("=", 1)[1].strip())
                    except (ValueError, IndexError):
                        pass
                    break
        except:
            pass

        # Initialize dirty flags with the correct count
        self._start_univ_dirty = [False] * self._num_outputs

        self.win = tk.Toplevel(parent)
        self.win.title("UzomaBox - %s [%s]" % (device_info.get("nick", self.ip), self.ip))
        self.win.geometry("680x740")
        self.win.resizable(True, True)

        # Always-visible status bar
        status_frame = ttk.Frame(self.win, relief=tk.SUNKEN, borderwidth=1)
        status_frame.pack(fill=tk.X, side=tk.TOP, padx=4, pady=(4, 0))
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
        self._build_artnet_tab()
        self._build_playback_record_tab()
        self._build_test_tab()
        self._build_status_tab()

        self.status_var = tk.StringVar(value="Ready")
        ttk.Label(self.win, textvariable=self.status_var, relief=tk.SUNKEN, anchor=tk.W).pack(
            fill=tk.X, side=tk.BOTTOM, padx=4, pady=(0, 4))

        self.after_id = None
        self._poll_status()

    # ---- Network Tab (Tab 1) ----
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
        ttk.Button(frame, text="OK & Reboot", command=self._save_ip).grid(row=1, column=2, padx=(8,0), pady=4)
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
        ttk.Label(frame, text="Outputs:").grid(row=7, column=0, sticky=tk.W, padx=(0,8), pady=2)
        self.outputs_var = tk.StringVar(value=str(self._num_outputs))
        ttk.Label(frame, textvariable=self.outputs_var).grid(row=7, column=1, sticky=tk.W, pady=2)

    # ---- LED Tab (Tab 2) ----
    def _build_led_tab(self):
        frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(frame, text="LEDs")
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
        # Table header
        columns_frame = ttk.Frame(frame)
        columns_frame.pack(fill=tk.X, pady=(0, 2))
        for txt, w in [("Out",5),("Active",6),("Start Univ",10),("Start Ch",8),("End Univ",10),("Subnet/Univ",10)]:
            ttk.Label(columns_frame, text=txt, font=("TkDefaultFont", 8, "bold"),
                      width=w).pack(side=tk.LEFT, padx=(2, 4))
        ttk.Separator(frame, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=(0, 4))
        self.output_active_vars = []
        self.start_univ_vars = []
        self.start_univ_old = []  # last valid values for revert on invalid input
        self.start_univ_entries = []  # Entry widgets for focus navigation
        self.end_univ_vars = []
        self.subnet_univ_vars = []
        for i in range(self._num_outputs):
            row_frame = ttk.Frame(frame)
            row_frame.pack(fill=tk.X, pady=1)
            ttk.Label(row_frame, text="%d" % (i + 1), width=5).pack(side=tk.LEFT, padx=(2, 0))
            av = tk.BooleanVar(value=True)
            self.output_active_vars.append(av)
            ttk.Checkbutton(row_frame, variable=av, width=4).pack(side=tk.LEFT, padx=(0, 4))
            sv = tk.StringVar(value=str(i * 3))
            self.start_univ_vars.append(sv)
            self.start_univ_old.append(str(i * 3))  # initial valid value for revert
            sc = ttk.Entry(row_frame, textvariable=sv, width=5)
            sc.pack(side=tk.LEFT, padx=(0, 4))
            sc.bind("<Return>", lambda e, idx=i: self._focus_next_univ(idx))
            sc.bind("<Down>", lambda e, idx=i: self._focus_next_univ(idx))
            sc.bind("<Up>", lambda e, idx=i: self._focus_prev_univ(idx))
            sc.bind("<Tab>", lambda e, idx=i: self._focus_next_univ(idx))
            sc.bind("<Shift-Tab>", lambda e, idx=i: self._focus_prev_univ(idx))
            sc.bind("<FocusIn>", lambda e, idx=i: self._mark_univ_dirty(idx))
            self.start_univ_entries.append(sc)
            ttk.Label(row_frame, text="1", width=8, anchor=tk.CENTER,
                      font=("Consolas", 9)).pack(side=tk.LEFT, padx=(0, 4))
            euv = tk.StringVar(value="--")
            self.end_univ_vars.append(euv)
            ttk.Label(row_frame, textvariable=euv, width=10, anchor=tk.CENTER,
                      font=("Consolas", 9)).pack(side=tk.LEFT, padx=(0, 4))
            suv = tk.StringVar(value="--")
            self.subnet_univ_vars.append(suv)
            ttk.Label(row_frame, textvariable=suv, width=10, anchor=tk.CENTER,
                      font=("Consolas", 9)).pack(side=tk.LEFT)
        self.led_width_var.trace_add("write", lambda *a: self._update_all_univ_ranges())
        self.color_order_var.trace_add("write", lambda *a: self._on_color_order_changed())
        # Add trace to all output active checkboxes
        for av in self.output_active_vars:
            av.trace_add("write", lambda *a: self._on_output_active_changed())
        self._update_all_univ_ranges()
        ttk.Button(frame, text="OK & Reboot",
                   command=self._save_led).pack(pady=12)

    def _compute_univ_range(self, led_width, start_univ):
        try:
            pixels = int(led_width)
            start = int(start_univ)
        except (ValueError, TypeError):
            return ("--", "--")
        total_ch = pixels * 3
        end_ch = ((total_ch - 1) % 512) + 1
        end_univ = start + (total_ch - 1) // 512
        return (str(end_univ), str(end_ch))

    def _on_start_univ_changed(self, idx):
        self._start_univ_dirty[idx] = True
        self._update_univ_range(idx)

    def _mark_univ_dirty(self, idx):
        """Mark row dirty when focused to prevent status poll from overwriting."""
        self._start_univ_dirty[idx] = True

    def _validate_start_univ(self, idx):
        """Validate start universe on focus-out / Enter. Revert to old value if invalid."""
        val = self.start_univ_vars[idx].get().strip()
        try:
            n = int(val)
            if 0 <= n <= 255:
                # Valid — mark dirty, update old value, refresh derived fields
                self.start_univ_old[idx] = val
                self._on_start_univ_changed(idx)
                return
        except (ValueError, TypeError):
            pass
        # Invalid — revert to last valid value
        self.start_univ_vars[idx].set(self.start_univ_old[idx])

    def _focus_next_univ(self, idx):
        """Validate current Entry, then focus the next one down."""
        self._validate_start_univ(idx)
        nxt = idx + 1
        if nxt < len(self.start_univ_entries):
            e = self.start_univ_entries[nxt]
            e.focus_set()
            e.selection_range(0, 'end')
        return 'break'

    def _focus_prev_univ(self, idx):
        """Validate current Entry, then focus the previous one up."""
        self._validate_start_univ(idx)
        prv = idx - 1
        if prv >= 0:
            e = self.start_univ_entries[prv]
            e.focus_set()
            e.selection_range(0, 'end')
        return 'break'

    def _update_univ_range(self, idx):
        if idx >= len(self.start_univ_vars):
            return
        led_w = self.led_width_var.get()
        start_u = self.start_univ_vars[idx].get()
        end_u, end_ch = self._compute_univ_range(led_w, start_u)
        self.end_univ_vars[idx].set(end_u)
        # Compute ArtNet subnet and universe from start universe
        try:
            su = int(start_u)
            subnet = (su >> 4) & 0x0F
            univ = su & 0x0F
            self.subnet_univ_vars[idx].set("%d:%d" % (subnet, univ))
        except (ValueError, TypeError):
            self.subnet_univ_vars[idx].set("--")

    def _on_color_order_changed(self):
        """Mark color order as dirty so status poll doesn't overwrite it."""
        self._color_order_dirty = True

    def _on_output_active_changed(self):
        """Mark output active as dirty so status poll doesn't overwrite it."""
        self._output_active_dirty = True

    def _update_all_univ_ranges(self):
        count = min(self._num_outputs, len(self.start_univ_vars))
        for i in range(count):
            self._update_univ_range(i)

    # ---- ArtNet Tab (Tab 3) ----
    def _build_artnet_tab(self):
        frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(frame, text="ArtNet")
        ttk.Label(frame, text="ArtNet Mode", font=("TkDefaultFont", 12, "bold")).pack(anchor=tk.W, pady=(0,8))
        self.artnet_btn = ttk.Button(frame, text="▶ Start ArtNet Mode", command=self._start_artnet, width=25)
        self.artnet_btn.pack(anchor=tk.W, pady=(0,12))
        fps_frame = ttk.LabelFrame(frame, text="Live ArtNet FPS", padding=8)
        fps_frame.pack(fill=tk.X)
        self.artnet_fps_var = tk.StringVar(value="0 FPS")
        ttk.Label(fps_frame, textvariable=self.artnet_fps_var,
                  font=("Consolas", 28, "bold")).pack(anchor=tk.CENTER, pady=8)

    def _start_artnet(self):
        self._cmd_send("MODE:artnet")
        self.status_var.set("Switched to ArtNet mode")
        self.log("ArtNet mode activated on %s" % self.ip)

    # ---- Playback / Record Tab (Tab 4) ----
    def _build_playback_record_tab(self):
        frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(frame, text="Playback/Record")

        # Top: mode buttons
        mode_frame = ttk.Frame(frame)
        mode_frame.pack(fill=tk.X, pady=(0,8))
        self.playback_btn = ttk.Button(mode_frame, text="▶ Playback Mode", command=self._start_playback, width=16)
        self.playback_btn.pack(side=tk.LEFT, padx=(0,8))
        self.record_mode_btn = ttk.Button(mode_frame, text="● Record Mode", command=self._start_record, width=16)
        self.record_mode_btn.pack(side=tk.LEFT)

        # Recording controls
        rec_frame = tk.LabelFrame(frame, text="Recording", padx=6, pady=6,
                                  font=("TkDefaultFont", 9, "bold"))
        rec_frame.pack(fill=tk.X, pady=(0,8))
        self.rec_frame = rec_frame

        # Row 0: FPS and buttons
        row0 = ttk.Frame(rec_frame)
        row0.pack(fill=tk.X, pady=(0,4))
        ttk.Label(row0, text="FPS:").pack(side=tk.LEFT, padx=(0,4))
        self.record_fps_var = tk.StringVar(value="30")
        ttk.Spinbox(row0, from_=5, to=60, textvariable=self.record_fps_var, width=6
                    ).pack(side=tk.LEFT, padx=(0,4))
        self.rec_start_btn = ttk.Button(row0, text="▶ Start", command=self._rec_start)
        self.rec_start_btn.pack(side=tk.LEFT, padx=(0,4))
        self.rec_stop_btn = ttk.Button(row0, text="■ Stop", command=self._rec_stop, state=tk.DISABLED)
        self.rec_stop_btn.pack(side=tk.LEFT, padx=(0,4))
        self.rec_status_var = tk.StringVar(value="Idle")
        ttk.Label(row0, textvariable=self.rec_status_var).pack(side=tk.LEFT, padx=(4,0))
        self.rec_timer_var = tk.StringVar(value="")
        ttk.Label(row0, textvariable=self.rec_timer_var, width=10).pack(side=tk.LEFT)
        ttk.Button(row0, text="OK & Reboot", command=self._save_record_fps).pack(side=tk.RIGHT)

        # Row 1: Start trigger
        row1 = ttk.Frame(rec_frame)
        row1.pack(fill=tk.X, pady=(0,4))
        ttk.Label(row1, text="Start:").pack(side=tk.LEFT, padx=(0,4))
        self.rec_start_mode_var = tk.StringVar(value="Immediate")
        start_modes = ["Immediate", "First Non-Zero", "Channel Change"]
        self.rec_start_mode_combo = ttk.Combobox(row1, textvariable=self.rec_start_mode_var,
                                                  values=start_modes, width=16, state="readonly")
        self.rec_start_mode_combo.pack(side=tk.LEFT, padx=(0,4))
        self.rec_start_mode_combo.bind("<<ComboboxSelected>>", lambda e: self._on_rec_trigger_change())
        ttk.Label(row1, text="Univ:").pack(side=tk.LEFT, padx=(4,2))
        self.rec_trig_univ_var = tk.StringVar(value="0")
        ttk.Spinbox(row1, from_=0, to=255, textvariable=self.rec_trig_univ_var, width=5
                    ).pack(side=tk.LEFT, padx=(0,4))
        ttk.Label(row1, text="Ch:").pack(side=tk.LEFT, padx=(0,2))
        self.rec_trig_ch_var = tk.StringVar(value="0")
        ttk.Spinbox(row1, from_=0, to=511, textvariable=self.rec_trig_ch_var, width=5
                    ).pack(side=tk.LEFT, padx=(0,4))
        self._rec_show_trig_fields()

        # Row 2: Stop trigger
        row2 = ttk.Frame(rec_frame)
        row2.pack(fill=tk.X, pady=(0,4))
        ttk.Label(row2, text="Stop:").pack(side=tk.LEFT, padx=(0,4))
        self.rec_stop_mode_var = tk.StringVar(value="Immediate")
        stop_modes = ["Immediate", "All Zero", "Timer"]
        self.rec_stop_mode_combo = ttk.Combobox(row2, textvariable=self.rec_stop_mode_var,
                                                 values=stop_modes, width=16, state="readonly")
        self.rec_stop_mode_combo.pack(side=tk.LEFT, padx=(0,4))
        self.rec_stop_mode_combo.bind("<<ComboboxSelected>>", lambda e: self._on_rec_trigger_change())
        ttk.Label(row2, text="Secs:").pack(side=tk.LEFT, padx=(4,2))
        self.rec_stop_secs_var = tk.StringVar(value="5")
        ttk.Spinbox(row2, from_=1, to=999, textvariable=self.rec_stop_secs_var, width=5
                    ).pack(side=tk.LEFT, padx=(0,4))
        self._rec_show_stop_fields()

        # Playback controls
        pb_frame = ttk.LabelFrame(frame, text="Playback", padding=6)
        pb_frame.pack(fill=tk.X, pady=(0,8))
        file_frame = ttk.Frame(pb_frame)
        file_frame.pack(fill=tk.X, pady=(0,4))
        ttk.Label(file_frame, text="File:").pack(side=tk.LEFT, padx=(0,4))
        self.play_file_var = tk.StringVar()
        ttk.Entry(file_frame, textvariable=self.play_file_var, width=22).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(file_frame, text="▶ Play", command=self._play_file, width=8).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(file_frame, text="■ Stop", command=self._stop, width=8).pack(side=tk.LEFT)

        list_frame = ttk.Frame(pb_frame)
        list_frame.pack(fill=tk.X, pady=(0,4))
        self.file_listbox = tk.Listbox(list_frame, height=4, font=("Consolas", 9))
        file_scroll = ttk.Scrollbar(list_frame, orient=tk.VERTICAL, command=self.file_listbox.yview)
        self.file_listbox.config(yscrollcommand=file_scroll.set)
        self.file_listbox.pack(side=tk.LEFT, fill=tk.X, expand=True)
        file_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.file_listbox.bind("<<ListboxSelect>>", self._on_file_select)
        btn_row = ttk.Frame(pb_frame)
        btn_row.pack(fill=tk.X, pady=(4,0))
        ttk.Button(btn_row, text="Refresh List", command=self._refresh_list).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(btn_row, text="Play All", command=self._play_all).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(btn_row, text="Delete Selected", command=self._delete_file).pack(side=tk.LEFT)

        speed_frame = ttk.Frame(pb_frame)
        speed_frame.pack(fill=tk.X, pady=(4,0))
        ttk.Label(speed_frame, text="Speed:").pack(side=tk.LEFT, padx=(0,4))
        self.speed_var = tk.DoubleVar(value=1.0)
        speed_scale = ttk.Scale(speed_frame, from_=0.2, to=2.0, orient=tk.HORIZONTAL,
                                 variable=self.speed_var, command=self._on_speed_change, length=150)
        speed_scale.pack(side=tk.LEFT, padx=(0,4))
        self.speed_label_var = tk.StringVar(value="1.00x")
        ttk.Label(speed_frame, textvariable=self.speed_label_var, width=8).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(speed_frame, text="Set", command=self._set_speed, width=6).pack(side=tk.LEFT)

        # Progress bar
        progress_frame = ttk.Frame(frame)
        progress_frame.pack(fill=tk.X)
        ttk.Label(progress_frame, text="Progress:").pack(side=tk.LEFT, padx=(0,4))
        self.progress_var = tk.DoubleVar(value=0.0)
        self.progress_bar = ttk.Progressbar(progress_frame, variable=self.progress_var, length=200, mode="determinate")
        self.progress_bar.pack(side=tk.LEFT, padx=(0,4))
        self.progress_label_var = tk.StringVar(value="-- / --")
        ttk.Label(progress_frame, textvariable=self.progress_label_var).pack(side=tk.LEFT)

    # ---- Test Tab (Tab 5) ----
    def _build_test_tab(self):
        frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(frame, text="Test")

        ttk.Label(frame, text="Test Mode", font=("TkDefaultFont", 12, "bold")).pack(anchor=tk.W, pady=(0,8))

        # Pattern selector
        pat_frame = ttk.LabelFrame(frame, text="Pattern", padding=6)
        pat_frame.pack(fill=tk.X, pady=(0,8))
        self.test_pattern_var = tk.StringVar(value="RGBW Cycle")
        self.test_pattern_var.trace_add("write", lambda *a: self._send_test_pattern())
        patterns = ["RGBW Cycle", "Color Fade", "Red", "Green", "Blue"]
        for pat in patterns:
            ttk.Radiobutton(pat_frame, text=pat, variable=self.test_pattern_var,
                            value=pat).pack(anchor=tk.W, padx=(8,0), pady=1)

        # Output selector
        out_frame = ttk.LabelFrame(frame, text="Output", padding=6)
        out_frame.pack(fill=tk.X, pady=(0,8))
        self.test_output_all = tk.BooleanVar(value=True)
        ttk.Radiobutton(out_frame, text="All outputs", variable=self.test_output_all,
                        value=True, command=self._on_test_output_toggle).pack(anchor=tk.W, padx=(8,0), pady=1)
        single_frame = ttk.Frame(out_frame)
        single_frame.pack(anchor=tk.W, padx=(8,0), pady=1)
        ttk.Radiobutton(single_frame, text="Single output", variable=self.test_output_all,
                        value=False, command=self._on_test_output_toggle).pack(side=tk.LEFT)
        self.test_output_combo = ttk.Combobox(single_frame,
                                              values=[str(i+1) for i in range(self._num_outputs)],
                                              width=4, state="readonly")
        self.test_output_combo.bind("<<ComboboxSelected>>", lambda e: self._send_test_output())
        self.test_output_combo.pack(side=tk.LEFT, padx=(4,0))
        self.test_output_combo.set("1")
        self.test_output_combo.config(state=tk.DISABLED)

        # Buttons
        btn_frame = ttk.Frame(frame)
        btn_frame.pack(fill=tk.X, pady=(8,0))
        self.test_start_btn = ttk.Button(btn_frame, text="▶ Start Test", command=self._start_test, width=16)
        self.test_start_btn.pack(side=tk.LEFT, padx=(0,8))
        ttk.Button(btn_frame, text="■ Stop Test", command=self._stop_test, width=16).pack(side=tk.LEFT)

    def _on_test_output_toggle(self):
        if self.test_output_all.get():
            self.test_output_combo.config(state=tk.DISABLED)
        else:
            self.test_output_combo.config(state="readonly")
        self._send_test_output()

    def _send_test_output(self):
        """Send the current output selection to the Teensy immediately."""
        if self.test_output_all.get():
            self._cmd_send("COMMAND:TEST_OUTPUT=255")
        else:
            strip_index = int(self.test_output_combo.get()) - 1
            if strip_index < self._num_outputs:
                self._cmd_send("COMMAND:TEST_OUTPUT=%s" % strip_index)

    def _send_test_pattern(self):
        """Send the current pattern to the Teensy immediately."""
        pattern_map = {"RGBW Cycle": "0", "Color Fade": "1", "Red": "2",
                       "Green": "3", "Blue": "4"}
        pat = pattern_map.get(self.test_pattern_var.get(), "0")
        self._cmd_send("COMMAND:TEST_PATTERN=%s" % pat)

    def _start_test(self):
        self._cmd_send("MODE:test")
        time.sleep(0.05)
        self._send_test_pattern()
        time.sleep(0.05)
        self._send_test_output()
        self.status_var.set("Test mode started")
        self.log("Test mode on %s: pattern=%s" % (self.ip, self.test_pattern_var.get()))

    def _stop_test(self):
        self._cmd_send("MODE:artnet")
        self.status_var.set("Test stopped, returning to ArtNet")
        self.log("Test stopped on %s" % self.ip)

    # ---- Status Tab (Tab 6) ----
    def _build_status_tab(self):
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

    # ---- Persistent TCP Helpers ----
    def _cmd(self, cmd):
        return self._tcp.send_and_recv(cmd)

    def _cmd_send(self, cmd):
        self._tcp.send_only(cmd)

    # ---- Actions ----
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

    def _save_led(self):
        color = self.color_order_var.get().strip()
        led_w = self.led_width_var.get().strip()
        univ = ",".join(v.get() for v in self.start_univ_vars)
        outputs = ",".join("1" if v.get() else "0" for v in self.output_active_vars)
        # Send non-reboot commands first
        self._cmd_send("CONFIG:color_order=%s" % color)
        time.sleep(0.05)
        # Send output_active before reboot-causing commands
        self._cmd_send("CONFIG:output_active=%s" % outputs)
        time.sleep(0.05)
        # Reboot-causing commands (whichever is last triggers the reboot)
        self._cmd_send("CONFIG:start_universe=%s" % univ)
        time.sleep(0.05)
        self._cmd_send("CONFIG:led_width=%s" % led_w)
        self._start_univ_dirty = [False] * self._num_outputs
        self.log("LED settings updated on %s (rebooting)" % self.ip)
        self.win.destroy()

    def _start_playback(self):
        self._cmd_send("PLAY:SEQUENCE")
        self.status_var.set("Switched to Playback mode")
        self.log("Playback mode on %s" % self.ip)

    def _start_record(self):
        self._cmd_send("MODE:record")
        self.status_var.set("Switched to Record mode")
        self.log("Record mode on %s" % self.ip)

    def _save_record_fps(self):
        fps = self.record_fps_var.get().strip()
        self._cmd_send("CONFIG:record_fps=%s" % fps)
        self.log("Record FPS updated on %s (rebooting)" % self.ip)
        self.win.destroy()

    def _rec_show_trig_fields(self):
        """Show/hide trigger fields based on start mode selection."""
        pass  # Fields are always visible, user fills them when needed

    def _rec_show_stop_fields(self):
        """Show/hide stop timer field based on stop mode selection."""
        pass

    def _on_rec_trigger_change(self):
        """Called when start or stop trigger mode changes."""
        pass

    def _rec_start(self):
        # Send trigger mode config before starting
        mode_map = {"Immediate": "0", "First Non-Zero": "1", "Channel Change": "2"}
        start_mode = mode_map.get(self.rec_start_mode_var.get(), "0")
        self._cmd_send("REC:START_MODE=%s" % start_mode)
        time.sleep(0.05)
        stop_map = {"Immediate": "0", "All Zero": "1", "Timer": "2"}
        stop_mode = stop_map.get(self.rec_stop_mode_var.get(), "0")
        self._cmd_send("REC:STOP_MODE=%s" % stop_mode)
        time.sleep(0.05)
        self._cmd_send("REC:TRIGGER_UNIV=%s" % self.rec_trig_univ_var.get())
        time.sleep(0.05)
        self._cmd_send("REC:TRIGGER_CH=%s" % self.rec_trig_ch_var.get())
        time.sleep(0.05)
        self._cmd_send("REC:STOP_SECS=%s" % self.rec_stop_secs_var.get())
        time.sleep(0.05)

        # Only send REC:START for immediate mode; otherwise just arm
        if start_mode == "0":
            self._cmd_send("REC:START")
            self.log("Recording started on %s" % self.ip)
        else:
            self._cmd_send("REC:ARM")
            self.log("Recording armed (trigger) on %s" % self.ip)
        self.rec_start_btn.config(state=tk.DISABLED)
        self.rec_stop_btn.config(state=tk.NORMAL)
        self.rec_status_var.set("Recording...")

    def _rec_stop(self):
        self._cmd_send("REC:STOP")
        self.rec_start_btn.config(state=tk.NORMAL)
        self.rec_stop_btn.config(state=tk.DISABLED)
        self.rec_status_var.set("Idle")
        self.log("Recording stopped on %s" % self.ip)

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
        speed = max(0.2, min(2.0, speed))
        self._cmd_send("SPEED:%.2f" % speed)
        self.log("Speed %.2fx on %s" % (speed, self.ip))

    def _refresh_list(self):
        lines = self._cmd("LIST")
        files = []
        for l in lines:
            if l.startswith("OK:") or l.startswith("END:"):
                continue
            if "=" in l:
                continue
            if not l.strip():
                continue
            files.append(l)
        self._file_list = sorted(files)
        self.file_listbox.delete(0, tk.END)
        for fn in self._file_list:
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
        if "=" not in line:
            return
        k, v = line.split("=", 1)
        k = k.strip()
        v = v.strip()
        self._status_data[k] = v
        if k == "mac":
            self.mac_var.set(v)
        elif k == "color_order":
            if not self._color_order_dirty:
                self.color_order_var.set(v)
        elif k == "led_width":
            self.led_width_var.set(v)
        elif k == "start_universe":
            parts = v.split(",")
            max_i = min(self._num_outputs, len(self.start_univ_vars))
            for i, p in enumerate(parts):
                if i < max_i and not self._start_univ_dirty[i]:
                    self.start_univ_vars[i].set(p.strip())
            self._update_all_univ_ranges()
        elif k == "output_active":
            if not self._output_active_dirty:
                parts = v.split(",")
                for i, p in enumerate(parts):
                    if i < len(self.output_active_vars):
                        self.output_active_vars[i].set(p.strip() == "1")
        elif k == "record_fps":
            self.record_fps_var.set(v)
        elif k == "output_count":
            try:
                new_count = int(v)
                if new_count != self._num_outputs:
                    self._num_outputs = new_count
                    self.outputs_var.set(str(self._num_outputs))
                    self.test_output_combo['values'] = [str(i+1) for i in range(new_count)]
            except (ValueError, IndexError):
                pass
        elif k == "mode":
            self.mode_var = tk.StringVar(value=v)
            if v.lower() == "record":
                if not self._breath_active and hasattr(self, 'rec_frame'):
                    self._breath_active = True
                    self._breath_val = 0.0
                    self._breathe_animate()
            else:
                if self._breath_active:
                    self._breath_active = False
                    if self._breath_after_id:
                        try:
                            self.win.after_cancel(self._breath_after_id)
                        except:
                            pass
                        self._breath_after_id = None
                    try:
                        self.rec_frame.configure(background='')
                    except:
                        pass
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
                # Do nothing — slider thumb and label are user-only
                # The always-visible bar is handled below
                pass
            except:
                pass
        elif k == "file_pos" or k == "file_total":
            self._update_progress()
        elif k == "artnet_fps":
            try:
                fps = int(v)
                self.artnet_fps_var.set("%d FPS" % fps)
            except:
                pass

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

    def _poll_status(self):
        try:
            if self.win.winfo_exists():
                try:
                    lines = self._cmd("STATUS")
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
        self._conn_indicator.delete("all")
        self._conn_indicator.create_oval(1, 1, 13, 13, fill="green", outline="black")

    def _set_conn_red(self):
        self._conn_indicator.delete("all")
        self._conn_indicator.create_oval(1, 1, 13, 13, fill="red", outline="black")

    def _breathe_animate(self):
        """Animate the recording frame background with a breathing gray pulse."""
        if not self._breath_active or not hasattr(self, 'rec_frame'):
            return
        import math
        self._breath_val += 0.08
        # Sine from 0 to π, mapping to gray range 100-155
        phase = (math.sin(self._breath_val) + 1.0) / 2.0  # 0..1
        gray = int(100 + phase * 55)
        hex_color = '#%02x%02x%02x' % (gray, gray, gray)
        try:
            self.rec_frame.configure(background=hex_color)
        except tk.TclError:
            pass
        self._breath_after_id = self.win.after(50, self._breathe_animate)

    def on_close(self):
        if self.after_id:
            try:
                self.win.after_cancel(self.after_id)
            except:
                pass
        if self._breath_after_id:
            try:
                self.win.after_cancel(self._breath_after_id)
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
        self._logo = None
        script_dir = os.path.dirname(os.path.abspath(__file__))
        logo_path = os.path.join(script_dir, "..", "black.png")
        if os.path.exists(logo_path):
            try:
                self._logo = tk.PhotoImage(file=logo_path).subsample(8, 8)
            except:
                pass
        self._build_ui()
        threading.Thread(target=self._thread_populate_interfaces, daemon=True).start()

    def _build_ui(self):
        main_frame = ttk.Frame(self.root, padding=8)
        main_frame.pack(fill=tk.BOTH, expand=True)
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
        self.bottom_var = tk.StringVar(value="No devices found")
        ttk.Label(main_frame, textvariable=self.bottom_var, relief=tk.SUNKEN, anchor=tk.W).pack(
            fill=tk.X, side=tk.BOTTOM, pady=(4, 0))
        self.context_menu = tk.Menu(self.root, tearoff=0)
        self.context_menu.add_command(label="Open Configuration", command=self._context_open)
        self.context_menu.add_command(label="Identify (Flash LED)", command=self._context_identify)
        self.context_menu.add_separator()
        self.context_menu.add_command(label="Delete from List", command=self._context_delete)
        self.tree.bind("<Button-3>", self._on_context_menu)

    def _thread_populate_interfaces(self):
        ifaces = get_interfaces()
        self.root.after(0, lambda: self._update_interfaces(ifaces))

    def _update_interfaces(self, ifaces):
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
        self.bottom_var.set("Searching adapters...")
        threading.Thread(target=self._thread_populate_interfaces, daemon=True).start()

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
                dev.get("model", "--"), dev.get("nick", "--"), dev.get("ip", "--"),
                dev.get("fw", "--"), dev.get("temp", "--")))
        n = len(devices)
        self.status_label.config(text="Found %d controller(s)" % n)
        self.bottom_var.set("%d device(s) discovered" % n)

    def _sort_by(self, col):
        items = [(self.tree.set(child, col), child) for child in self.tree.get_children("")]
        items.sort()
        for idx, (val, child) in enumerate(items):
            self.tree.move(child, "", idx)

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