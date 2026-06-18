# UzomaBox Desktop Controller

Desktop application to control a **Teensy 4.1** running the **UzomaBox** firmware (ArtNet/Playback/Record LED controller over 8 × WS2811 strips).

## Requirements

- Python **3.6 or higher**
- No external packages — uses only `tkinter`, `socket`, `threading` from the standard library

## How to Run

```bash
cd UzomaBoxDesktop
python uzoma_box_desktop.py
```

Or double-click `uzoma_box_desktop.py` in Windows Explorer.

## Features

| Section | Feature |
|---------|---------|
| **Connection** | Connect/Disconnect by IP address; **Adapter** dropdown to bind to a specific network interface (shows each adapter's IP); real-time latency indicator (PING/PONG) |
| **Mode** | Radio buttons to switch between ArtNet, Playback, and Record |
| **Playback & File Mgmt** | Play single `.BIN` file, play all files in sequence, speed slider (0.05x–5.0x), progress bar with KB readout; file list with refresh and delete |
| **Recording** | Start/Stop recording, status display |
| **Configuration** | Edit IP, MAC, LED width, start universes, output mask, color order, recording FPS — Apply saves to SD card and reboots the Teensy |
| **Status** | Displays raw STATUS response from Teensy with a Refresh button |
| **Log** | Timestamped log of all sent/received TCP messages |

## TCP Protocol (port 8888)

| Command | Description |
|---------|-------------|
| `PING` | Keep-alive — Teensy responds `PONG` |
| `MODE:artnet` | Switch to ArtNet mode |
| `MODE:playback` | Switch to Playback mode |
| `MODE:record` | Switch to Record mode |
| `REC:START` | Begin recording ArtNet data to a new `.BIN` file |
| `REC:STOP` | Stop recording and close the file |
| `PLAY:<filename.BIN>` | Play a specific `.BIN` file |
| `PLAY:SEQUENCE` | Play all `.BIN` files in sequence |
| `STOP` | Stop any playback or recording |
| `SPEED:1.5` | Set playback speed multiplier (0.05–5.0, live, no reboot) |
| `LIST` | List all `.BIN` files on the SD card |
| `DELETE:filename.BIN` | Delete a `.BIN` file from the SD card |
| `STATUS` | Request status information (includes speed, fps, file progress) |
| `CONFIG:key=value` | Update a configuration value (reboots Teensy) — keys: `ip`, `mac`, `led_width`, `start_universe`, `output_active`, `color_order`, `record_fps` |

## Configuration Keys

| Key | Example | Description |
|-----|---------|-------------|
| `ip` | `192.168.0.211` | Static IP address |
| `mac` | `DE:AD:BE:EF:BE:ED` | MAC address |
| `led_width` | `512` | LEDs per strip |
| `start_universe` | `0,3,6,9,12,15,18,21` | Starting Art-Net universe per strip |
| `output_active` | `1,1,1,1,1,1,1,1` | Enable/disable each of 8 strip outputs |
| `color_order` | `grb` | RGB byte order (rgb, rbg, grb, gbr, brg, bgr) |
| `record_fps` | `30` | Recording frame rate (5–60 FPS) |

## Auto-connection

The app sends PING every 2 seconds and STATUS every 5 seconds automatically once connected. If the connection is lost, it attempts to reconnect with exponential backoff (1s → 2s → 4s → ... → 10s max).