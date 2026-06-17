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
| **Connection** | Connect/Disconnect by IP address, real-time latency indicator (PING/PONG) |
| **Mode** | Radio buttons to switch between ArtNet, Playback, and Record |
| **Recording** | Start/Stop recording, status display |
| **Playback** | Play a single `.BIN` file, play all files in sequence, Stop |
| **Configuration** | Edit IP, MAC, LED width, start universes, output mask — Apply saves to SD card and reboots the Teensy |
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
| `STATUS` | Request status information |
| `CONFIG:key=value` | Update a configuration value (reboots Teensy) |

## Auto-connection

The app sends PING every 2 seconds and STATUS every 5 seconds automatically once connected. If the connection is lost, it attempts to reconnect with exponential backoff (1s → 2s → 4s → ... → 10s max).