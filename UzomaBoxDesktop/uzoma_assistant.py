#!/usr/bin/env python3
"""
UzomaBox Assistant
===================
Multi-device controller for UzomaBox Teensy 4.1 LED controllers.

Inspired by the Advatek PixLite Assistant workflow:
  - Discovers all UzomaBox controllers on the LAN via UDP broadcast
  - Shows them in a sortable table (Model, Nickname, IP, FW, Temp)
  - Double-click opens a per-device tabbed configuration window
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
from datetime import datetime

# ============================================================================
# Configuration
# ============================================================================
DISCOVERY_PORT   = 7777
DISCOVERY_TIMEOUT = 2.0    # seconds to wait for discovery responses
TCP_PORT         = 8888

# ============================================================================
# UDP Discovery
# ============================================================================
def discover_controllers(bind_ip=None, timeout=DISCOVERY_TIMEOUT):
    """
    Send UDP broadcast discovery request and collect responses.
    Returns list of dicts: {model, nick, ip, fw, temp}
    """
    devices = []
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(timeout)

    if bind_ip:
        sock.bind((bind_ip, 0))

    try:
        # Send broadcast
        sock.sendto(b"UZOMA:SEARCH", ("255.255.255.255", DISCOVERY_PORT))

        while True:
            try:
                data, addr = sock.recvfrom(1024)
                line = data.decode("utf-8", errors="replace").strip()
                # Parse key=value pairs separated by commas
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
# TCP Client (single connection, blocking per-command)
# ============================================================================
class TcpClientSync:
    """Synchronous TCP client for a single command/response cycle."""

    def __init__(self, host, port=TCP_PORT, timeout=5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None

    def connect(self):
        if self.sock:
            return
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect((self.host, self.port))
        # Drain the OK:connected banner
        try:
            self.sock.recv(1024)
        except socket.timeout:
            pass

    def send(self, cmd):
        """Send a command and return the full response (all lines)."""
        if not self.sock:
            self.connect()
        self.sock.sendall((cmd + "\n").encode("utf-8"))
        # Read response lines
        lines = []
        buf = b""
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                buf += chunk
                # Process complete lines
                while b"\n" in buf or b"\r" in buf:
                    idx = -1
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
                if b"\n" not in buf and b"\r" not in buf:
                    break
        except socket.timeout:
            pass
        return lines

    def close(self):
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
    """Return list of (ip, name) tuples for all non-loopback IPv4 interfaces."""
    interfaces = []

    # Strategy 1: POSIX (if_nameindex + getaddrinfo)
    try:
        for if_idx, if_name in socket.if_nameindex():
            try:
                addrs = socket.getaddrinfo(if_name, None, socket.AF_INET)
                for addr in addrs:
                    ip = addr[4][0]
                    if not ip.startswith("127."):
                        interfaces.append((ip, if_name))
                        break
            except (socket.gaierror, OSError):
                pass
    except Exception:
        pass

    # Deduplicate
    seen = set()
    unique = []
    for ip, name in interfaces:
        if ip not in seen:
            seen.add(ip)
            unique.append((ip, name))
    if unique:
        return unique

    # Strategy 2: Windows ipconfig
    try:
        output = subprocess.check_output(
            "ipconfig", shell=True, stderr=subprocess.DEVNULL,
            timeout=5
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

    # Strategy 3: gethostbyname fallback
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
    """Tabbed configuration window for a single UzomaBox device."""

    def __init__(self, parent, device_info, log_callback):
        self.device = device_info
        self.log = log_callback
        self.ip = device_info["ip"]
        self._status_data = {}  # parsed key=value from STATUS
        self._file_list = []
        self._listening_list = False
        self._progress_active = False

        # Build window
        self.win = tk.Toplevel(parent)
        self.win.title("UzomaBox - %s [%s]" % (device_info.get("nick", self.ip), self.ip))
        self.win.geometry("680x600")
        self.win.resizable(True, True)

        # Notebook (tabs)
        self.notebook = ttk.Notebook(self.win, padding=4)
        self.notebook.pack(fill=tk.BOTH, expand=True)

        self._build_network_tab()
        self._build_led_tab()
        self._build_control_tab()
        self._build_playback_tab()
        self._build_misc_tab()

        # Status bar at bottom
        self.status_var = tk.StringVar(value="Ready")
        ttk.Label(self.win, textvariable=self.status_var, relief=tk.SUNKEN, anchor=tk.W).pack(
            fill=tk.X, side=tk.BOTTOM, padx=4, pady=(0, 4))

        # Fetch initial status from device
        self.after_id = None
        self._poll_status()

    # ---- Network Tab (Tab 1) -----------------------------------------------

    def _build_network_tab(self):
        frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(frame, text="Network")

        # Nickname
        ttk.Label(frame, text="Nickname:").grid(row=0, column=0, sticky=tk.W, padx=(0,8), pady=4)
        self.nick_var = tk.StringVar(value=self.device.get("nick", ""))
        ttk.Entry(frame, textvariable=self.nick_var, width=30).grid(row=0, column=1, sticky=tk.W, pady=4)
        ttk.Button(frame, text="Save", command=self._save_nickname).grid(row=0, column=2, padx=(8,0), pady=4)

        # IP Address
        ttk.Label(frame, text="IP Address:").grid(row=1, column=0, sticky=tk.W, padx=(0,8), pady=4)
        self.ip_var = tk.StringVar(value=self.ip)
        ttk.Entry(frame, textvariable=self.ip_var, width=30).grid(row=1, column=1, sticky=tk.W, pady=4)
        ttk.Button(frame, text="Apply & Reboot", command=self._save_ip).grid(row=1, column=2, padx=(8,0), pady=4)

        # MAC (read-only)
        ttk.Label(frame, text="MAC Address:").grid(row=2, column=0, sticky=tk.W, padx=(0,8), pady=4)
        self.mac_var = tk.StringVar(value="--")
        ttk.Label(frame, textvariable=self.mac_var, width=30, relief=tk.SUNKEN, anchor=tk.W).grid(
            row=2, column=1, sticky=tk.W, pady=4)

        ttk.Separator(frame, orient=tk.HORIZONTAL).grid(row=3, column=0, columnspan=3, sticky=tk.EW, pady=8)

        # Status fields (read-only)
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

        # LEDs per strip
        ttk.Label(frame, text="LEDs per Strip:").grid(row=0, column=0, sticky=tk.W, padx=(0,8), pady=4)
        self.led_width_var = tk.StringVar(value="512")
        ttk.Entry(frame, textvariable=self.led_width_var, width=12).grid(row=0, column=1, sticky=tk.W, pady=4)

        # Color order
        ttk.Label(frame, text="Color Order:").grid(row=1, column=0, sticky=tk.W, padx=(0,8), pady=4)
        self.color_order_var = tk.StringVar(value="rgb")
        ttk.Combobox(frame, textvariable=self.color_order_var,
                      values=["rgb", "rbg", "grb", "gbr", "brg", "bgr"],
                      width=10, state="readonly").grid(row=1, column=1, sticky=tk.W, pady=4)

        # Start universes
        ttk.Label(frame, text="Start Univs:").grid(row=2, column=0, sticky=tk.W, padx=(0,8), pady=4)
        self.start_univ_var = tk.StringVar(value="0,3,6,9,12,15,18,21")
        ttk.Entry(frame, textvariable=self.start_univ_var, width=30).grid(row=2, column=1, sticky=tk.W, pady=4)

        # Output active (8 checkboxes)
        ttk.Label(frame, text="Outputs:").grid(row=3, column=0, sticky=tk.NW, padx=(0,8), pady=4)
        self.output_vars = []
        out_frame = ttk.Frame(frame)
        out_frame.grid(row=3, column=1, sticky=tk.W, pady=4)
        for i in range(8):
            var = tk.BooleanVar(value=True)
            self.output_vars.append(var)
            ttk.Checkbutton(out_frame, text="%d" % (i+1), variable=var).pack(side=tk.LEFT, padx=2)

        ttk.Button(frame, text="Apply LED Settings (reboot)", command=self._save_led).grid(
            row=4, column=0, columnspan=2, pady=12)

    # ---- Control Tab (Tab 3) ------------------------------------------------

    def _build_control_tab(self):
        frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(frame, text="Control")

        # Mode selector
        ttk.Label(frame, text="Mode:").grid(row=0, column=0, sticky=tk.W, padx=(0,8), pady=4)
        self.mode_var = tk.StringVar(value="artnet")
        mode_frame = ttk.Frame(frame)
        mode_frame.grid(row=0, column=1, sticky=tk.W, pady=4)
        for mode in [("ArtNet", "artnet"), ("Playback", "playback"), ("Record", "record")]:
            ttk.Radiobutton(mode_frame, text=mode[0], variable=self.mode_var,
                             value=mode[1], command=self._change_mode).pack(side=tk.LEFT, padx=(0,12))

        # Recording FPS
        ttk.Label(frame, text="Record FPS:").grid(row=1, column=0, sticky=tk.W, padx=(0,8), pady=4)
        self.record_fps_var = tk.StringVar(value="30")
        ttk.Spinbox(frame, from_=5, to=60, textvariable=self.record_fps_var, width=6).grid(row=1, column=1, sticky=tk.W, pady=4)

        # Recording controls
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

    # ---- Playback Tab (Tab 4) -----------------------------------------------

    def _build_playback_tab(self):
        frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(frame, text="Playback")

        # File controls
        file_frame = ttk.Frame(frame)
        file_frame.pack(fill=tk.X, pady=(0,6))

        ttk.Label(file_frame, text="File:").pack(side=tk.LEFT, padx=(0,4))
        self.play_file_var = tk.StringVar()
        ttk.Entry(file_frame, textvariable=self.play_file_var, width=22).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(file_frame, text="▶ Play", command=self._play_file, width=8).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(file_frame, text="■ Stop", command=self._stop, width=8).pack(side=tk.LEFT)

        # File list
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

        # Speed slider
        speed_frame = ttk.Frame(frame)
        speed_frame.pack(fill=tk.X)
        ttk.Label(speed_frame, text="Speed:").pack(side=tk.LEFT, padx=(0,4))
        self.speed_var = tk.DoubleVar(value=1.0)
        speed_scale = ttk.Scale(speed_frame, from_=0.05, to=5.0, orient=tk.HORIZONTAL,
                                 variable=self.speed_var, command=self._on_speed_change,
                                 length=150)
        speed_scale.pack(side=tk.LEFT, padx=(0,4))
        self.speed_label_var = tk.StringVar(value="1.00x")
        ttk.Label(speed_frame, textvariable=self.speed_label_var, width=8).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(speed_frame, text="Set", command=self._set_speed, width=6).pack(side=tk.LEFT)

        # Progress bar
        progress_frame = ttk.Frame(frame)
        progress_frame.pack(fill=tk.X, pady=(6,0))
        ttk.Label(progress_frame, text="Progress:").pack(side=tk.LEFT, padx=(0,4))
        self.progress_var = tk.DoubleVar(value=0.0)
        self.progress_bar = ttk.Progressbar(progress_frame, variable=self.progress_var, length=200, mode="determinate")
        self.progress_bar.pack(side=tk.LEFT, padx=(0,4))
        self.progress_label_var = tk.StringVar(value="-- / --")
        ttk.Label(progress_frame, textvariable=self.progress_label_var).pack(side=tk.LEFT)

    # ---- Misc / Status Tab (Tab 5) -----------------------------------------

    def _build_misc_tab(self):
        frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(frame, text="Status")

        # Raw status display
        ttk.Label(frame, text="Device Status:").pack(anchor=tk.W)

        self.status_text = tk.Text(frame, height=12, wrap=tk.NONE, font=("Consolas", 9))
        status_scroll = ttk.Scrollbar(frame, orient=tk.VERTICAL, command=self.status_text.yview)
        self.status_text.config(yscrollcommand=status_scroll.set)
        self.status_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        status_scroll.pack(side=tk.RIGHT, fill=tk.Y)

        btn_frame = ttk.Frame(frame)
        btn_frame.pack(fill=tk.X, pady=(6,0))
        ttk.Button(btn_frame, text="Refresh Status", command=self._request_status).pack(side=tk.LEFT, padx=(0,4))
        ttk.Button(btn_frame, text="Identify (Flash LED)", command=self._identify).pack(side=tk.LEFT, padx=(0,4))
        self.close_btn = ttk.Button(btn_frame, text="Close", command=self.win.destroy)
        self.close_btn.pack(side=tk.RIGHT)

    # ---- TCP Helpers -------------------------------------------------------

    def _tcp_cmd(self, cmd):
        """Send a TCP command to the device and return response lines."""
        client = TcpClientSync(self.ip)
        try:
            lines = client.send(cmd)
            client.close()
            return lines
        except Exception as e:
            self.log("TCP error to %s: %s" % (self.ip, e))
            return []

    def _send_and_close(self, cmd):
        """Send a single command, ignore response (fire-and-forget)."""
        try:
            c = TcpClientSync(self.ip, timeout=1.0)
            c.send(cmd)
            c.close()
        except:
            pass

    # ---- Network Tab Actions -----------------------------------------------

    def _save_nickname(self):
        nick = self.nick_var.get().strip()
        if nick:
            self._send_and_close("CONFIG:nickname=%s" % nick)
            self.status_var.set("Nickname saved (live)")
            self.log("Set nickname on %s: %s" % (self.ip, nick))

    def _save_ip(self):
        new_ip = self.ip_var.get().strip()
        if not new_ip:
            return
        if not messagebox.askyesno("Change IP", "Changing IP to %s\nDevice will reboot.\nContinue?" % new_ip):
            return
        self._send_and_close("CONFIG:ip=%s" % new_ip)
        self.status_var.set("IP changed, device rebooting...")
        self.log("Changed IP on %s to %s (rebooting)" % (self.ip, new_ip))

    # ---- LED Tab Actions ---------------------------------------------------

    def _save_led(self):
        led_w = self.led_width_var.get().strip()
        color = self.color_order_var.get().strip()
        univ = self.start_univ_var.get().strip()
        outputs = ",".join("1" if v.get() else "0" for v in self.output_vars)

        cmd = "CONFIG:color_order=%s" % color
        # Send color order live first, then queue the reboot-required ones
        self._send_and_close(cmd)
        time.sleep(0.05)
        self._send_and_close("CONFIG:led_width=%s" % led_w)
        self._send_and_close("CONFIG:start_universe=%s" % univ)
        self._send_and_close("CONFIG:output_active=%s" % outputs)
        self.status_var.set("LED settings sent (rebooting)")
        self.log("LED settings updated on %s" % self.ip)

    # ---- Control Tab Actions -----------------------------------------------

    def _change_mode(self):
        mode = self.mode_var.get()
        self._send_and_close("MODE:%s" % mode)
        self.status_var.set("Switching to %s mode" % mode)

    def _save_record_fps(self):
        fps = self.record_fps_var.get().strip()
        self._send_and_close("CONFIG:record_fps=%s" % fps)
        self.status_var.set("Record FPS updated (rebooting)")

    def _rec_start(self):
        self._send_and_close("REC:START")
        self.rec_start_btn.config(state=tk.DISABLED)
        self.rec_stop_btn.config(state=tk.NORMAL)
        self.rec_status_var.set("Recording...")
        self.log("Recording started on %s" % self.ip)

    def _rec_stop(self):
        self._send_and_close("REC:STOP")
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
        self._send_and_close("PLAY:%s" % fn)
        self.log("Playing %s on %s" % (fn, self.ip))

    def _play_all(self):
        self._send_and_close("PLAY:SEQUENCE")
        self.log("Play all on %s" % self.ip)

    def _stop(self):
        self._send_and_close("STOP")
        self.rec_start_btn.config(state=tk.NORMAL)
        self.rec_stop_btn.config(state=tk.DISABLED)
        self.rec_status_var.set("Idle")
        self.log("Stop on %s" % self.ip)

    def _on_speed_change(self, *args):
        self.speed_label_var.set("%.2fx" % self.speed_var.get())

    def _set_speed(self):
        speed = self.speed_var.get()
        speed = max(0.05, min(5.0, speed))
        self._send_and_close("SPEED:%.2f" % speed)
        self.log("Speed %.2fx on %s" % (speed, self.ip))

    def _refresh_list(self):
        client = TcpClientSync(self.ip, timeout=4.0)
        try:
            lines = client.send("LIST")
            files = [l for l in lines if not l.startswith("OK:") and not l.startswith("END:")]
            self._file_list = files
            self.file_listbox.delete(0, tk.END)
            for fn in sorted(files):
                self.file_listbox.insert(tk.END, fn)
            client.close()
        except Exception as e:
            self.log("LIST error: %s" % e)

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
        self._send_and_close("DELETE:%s" % fn)
        self.log("Deleted %s from %s" % (fn, self.ip))
        self._refresh_list()

    # ---- Misc / Status Tab Actions -----------------------------------------

    def _identify(self):
        self._send_and_close("IDENTIFY")
        self.status_var.set("Identify sent (LED flashes)")
        self.log("Identify on %s" % self.ip)

    def _request_status(self):
        client = TcpClientSync(self.ip, timeout=4.0)
        try:
            lines = client.send("STATUS")
            self.status_text.delete("1.0", tk.END)
            for line in lines:
                self.status_text.insert(tk.END, line + "\n")
                self._parse_status_line(line)
            client.close()
            self.status_var.set("Status updated")
        except Exception as e:
            self.log("STATUS error: %s" % e)

    def _parse_status_line(self, line):
        """Parse key=value from a STATUS response line."""
        if "=" not in line:
            return
        k, v = line.split("=", 1)
        k = k.strip()
        v = v.strip()
        self._status_data[k] = v
        if k == "mac":
            self.mac_var.set(v)
        elif k == "color_order":
            self.color_order_var.set(v)
        elif k == "led_width":
            self.led_width_var.set(v)
        elif k == "start_universe":
            self.start_univ_var.set(v)
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

    # ---- Periodic Polling --------------------------------------------------

    def _poll_status(self):
        """Poll STATUS every 5 seconds while window is open."""
        try:
            if self.win.winfo_exists():
                client = TcpClientSync(self.ip, timeout=3.0)
                try:
                    lines = client.send("STATUS")
                    for line in lines:
                        self._parse_status_line(line)
                    client.close()
                except:
                    pass
                self.after_id = self.win.after(5000, self._poll_status)
        except tk.TclError:
            pass

    def on_close(self):
        if self.after_id:
            try:
                self.win.after_cancel(self.after_id)
            except:
                pass
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
        self._devices = []       # list of device dicts
        self._device_windows = {}  # ip -> DeviceConfigWindow
        self._searching = False

        self._build_ui()
        # Populate adapter list asynchronously
        self.root.after(100, self._populate_interfaces)

    # ---- UI BUILDING -------------------------------------------------------

    def _build_ui(self):
        main_frame = ttk.Frame(self.root, padding=8)
        main_frame.pack(fill=tk.BOTH, expand=True)

        # Toolbar
        toolbar = ttk.Frame(main_frame)
        toolbar.pack(fill=tk.X, pady=(0, 6))

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

        # Scrollbars
        tree_frame = ttk.Frame(main_frame)
        tree_frame.pack(fill=tk.BOTH, expand=True)
        v_scroll = ttk.Scrollbar(tree_frame, orient=tk.VERTICAL, command=self.tree.yview)
        h_scroll = ttk.Scrollbar(tree_frame, orient=tk.HORIZONTAL, command=self.tree.xview)
        self.tree.config(yscrollcommand=v_scroll.set, xscrollcommand=h_scroll.set)
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        v_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        h_scroll.pack(side=tk.BOTTOM, fill=tk.X)

        # Double-click opens config window
        self.tree.bind("<Double-1>", self._on_device_double_click)

        # Status bar at bottom
        self.bottom_var = tk.StringVar(value="No devices found")
        ttk.Label(main_frame, textvariable=self.bottom_var, relief=tk.SUNKEN, anchor=tk.W).pack(
            fill=tk.X, side=tk.BOTTOM, pady=(4, 0))

        # Context menu
        self.context_menu = tk.Menu(self.root, tearoff=0)
        self.context_menu.add_command(label="Open Configuration", command=self._context_open)
        self.context_menu.add_command(label="Identify (Flash LED)", command=self._context_identify)
        self.context_menu.add_separator()
        self.context_menu.add_command(label="Delete from List", command=self._context_delete)
        self.tree.bind("<Button-3>", self._on_context_menu)  # Windows right-click

    # ---- Interface Population -----------------------------------------------

    def _populate_interfaces(self):
        """Fill the adapter dropdown."""
        ifaces = get_interfaces()
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

    # ---- Device Discovery ---------------------------------------------------

    def _search_devices(self):
        if self._searching:
            return
        self._searching = True
        self.search_btn.config(state=tk.DISABLED)
        self.status_label.config(text="Searching...")
        self.bottom_var.set("Scanning network for controllers...")

        # Run discovery in background thread
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

        # Update tree
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
        """Sort the tree by a given column."""
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
        # Find matching device data
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
        # Reuse an existing window if already open
        if ip in self._device_windows:
            try:
                self._device_windows[ip].win.lift()
                return
            except:
                del self._device_windows[ip]
        # Create new config window
        self._device_windows[ip] = DeviceConfigWindow(self.root, dev, self._log)
        self._log("Opened config for %s [%s]" % (dev.get("nick", ip), ip))

    def _context_identify(self):
        dev = self._get_selected_device()
        if not dev:
            return
        ip = dev["ip"]
        try:
            c = TcpClientSync(ip, timeout=2.0)
            c.send("IDENTIFY")
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