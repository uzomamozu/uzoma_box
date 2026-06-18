# UzomaBox

Teensy 4.1 multi-mode LED controller for 8 × WS2811 LED strips.

## Overview

UzomaBox is a firmware + desktop controller system for driving addressable LED strips over Ethernet. It supports three operating modes and runs on a **Teensy 4.1** with an **OctoWS2811** adaptor.

## Features

- **Art-Net mode** — Receive live pixel data over Art-Net UDP (port 6454) with configurable universe mapping for up to 8 strips
- **Playback mode** — Play pre-recorded `.BIN` files from SD card with looping, speed control (0.05x–5.0x), and progress tracking
- **Record mode** — Capture Art-Net streams and save them as `.BIN` files on the SD card at a configurable frame rate (5–60 FPS)
- **TCP remote control** — Full control via a desktop GUI (port 8888)
- **Configurable color order** — RGB, GRB, BGR, and all other permutations for different LED strip types

## Project Structure

| Path | Description |
|------|-------------|
| `UzomaBox/` | Teensy 4.1 firmware (Arduino / PlatformIO project) |
| `UzomaBoxDesktop/` | Desktop controller application (Python/Tkinter) |
| `VideoSDcard/` | Reference example from Paul Stoffregen's OctoWS2811 library |

### Firmware Modules (`UzomaBox/`)

| File | Description |
|------|-------------|
| `UzomaBox.ino` | Main loop, mode dispatch, TCP command handler |
| `Config.h/.cpp` | Load/save `CONFIG.TXT` from SD card |
| `SDManager.h/.cpp` | Buffered SD card I/O (512-byte blocks) |
| `LEDController.h/.cpp` | OctoWS2811 wrapper with color-order reordering |
| `ArtNetHandler.h/.cpp` | Art-Net UDP receiver with multi-universe frame assembly |
| `PlaybackController.h/.cpp` | .BIN file playback and recording with speed control |
| `TCPHandler.h/.cpp` | TCP server and command parsing |

## Getting Started

### Hardware Requirements

- Teensy 4.1 + OctoWS2811 Adaptor
- 8 × WS2811/WS2812 LED strips
- MicroSD card (FAT32 format)
- Ethernet connection to the same network as your lighting console or desktop

### Firmware Setup

1. Install [Arduino IDE](https://www.arduino.cc/en/software) with [Teensyduino](https://www.pjrc.com/teensy/teensyduino.html)
2. Open `UzomaBox/UzomaBox.ino`
3. Insert a microSD card and power on the Teensy — a default `CONFIG.TXT` will be created
4. Edit `CONFIG.TXT` on the SD card to set your desired IP address, MAC, and strip configuration
5. Reboot the Teensy

### Desktop App

```bash
cd UzomaBoxDesktop
python uzoma_box_desktop.py
```

No external dependencies required — uses only Python standard library.

### Default Configuration

| Parameter | Default |
|-----------|---------|
| IP | `192.168.0.211` |
| MAC | `DE:AD:BE:EF:BE:ED` |
| LEDs per strip | 512 |
| Mode | ArtNet |
| Color order | RGB |
| Recording FPS | 30 |
| Playback speed | 1.0x |

## TCP Protocol (port 8888)

| Command | Description |
|---------|-------------|
| `PING` | Keep-alive — responds `PONG` |
| `MODE:artnet` | Switch to ArtNet mode |
| `MODE:playback` | Switch to Playback mode |
| `MODE:record` | Switch to Record mode |
| `REC:START` | Begin recording to a new `.BIN` file |
| `REC:STOP` | Stop and close recording |
| `PLAY:<filename.BIN>` | Play a specific file (loops) |
| `PLAY:SEQUENCE` | Play all `.BIN` files in sequence (loops) |
| `STOP` | Stop playback/recording |
| `SPEED:1.5` | Set playback speed (0.05–5.0, live) |
| `LIST` | List all `.BIN` files on SD card |
| `DELETE:filename.BIN` | Delete a file |
| `STATUS` | Request full status |
| `CONFIG:key=value` | Update a configuration value (reboots) |

## License

This project is based on the OctoWS2811 library by Paul Stoffregen (PJRC).

- `VideoSDcard/VideoSDcard.ino` — Copyright (c) 2014 Paul Stoffregen, PJRC.COM, LLC (MIT License)
- `UzomaBox/` and `UzomaBoxDesktop/` — Original work