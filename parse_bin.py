#!/usr/bin/env python3
"""Parse a UzomaBox .BIN recording file and dump all frame headers."""
import sys
import struct
import math

if len(sys.argv) < 2:
    print(f"Usage: {sys.argv[0]} <file.BIN>")
    sys.exit(1)

path = sys.argv[1]
with open(path, 'rb') as f:
    data = f.read()

pos = 0
frame = 0
ftimes = []

print(f"File: {path}")
print(f"Size: {len(data)} bytes  ({len(data)/1024/1024:.1f} MB)")
print()

while pos < len(data):
    remaining = len(data) - pos
    if remaining < 1:
        break

    type_byte = data[pos]

    if frame == 0 and type_byte not in (ord('*'), ord('+'), ord('%')):
        if remaining >= 4:
            fc = struct.unpack('<I', data[pos:pos+4])[0]
            print(f"[HEADER] Total frames (from header): {fc}")
            pos += 4
            continue
        break

    if type_byte == ord('*'):
        if remaining < 5: break
        pix = struct.unpack('<H', data[pos+1:pos+3])[0]
        ft = struct.unpack('<H', data[pos+3:pos+5])[0]
        header_len = 5
    elif type_byte == ord('+'):
        if remaining < 7: break
        pix = struct.unpack('<H', data[pos+1:pos+3])[0]
        ft = struct.unpack('<I', data[pos+3:pos+7])[0]
        header_len = 7
    elif type_byte == ord('%'):
        if remaining < 3: break
        sz = struct.unpack('<H', data[pos+1:pos+3])[0]
        pos += 3 + sz * 2
        continue
    else:
        pos += 1
        continue

    ftimes.append(ft)
    pos += header_len + pix * 3
    frame += 1

print(f"Total frames: {frame}")
print()

if ftimes:
    total_us = sum(ftimes)
    total_sec = total_us / 1e6
    min_ft = min(ftimes)
    max_ft = max(ftimes)
    avg_ft = sum(ftimes) / len(ftimes)

    print(f"=== STORED FRAME TIMING ===")
    print(f"  Total:     {total_sec:.1f} seconds ({total_sec/60:.1f} min)")
    print(f"  Min:       {min_ft} us ({min_ft/1000:.1f} ms)")
    print(f"  Max:       {max_ft} us ({max_ft/1000:.1f} ms)")
    print(f"  Avg:       {avg_ft:.0f} us ({avg_ft/1000:.1f} ms)")
    print(f"  Eff FPS:   {1/(avg_ft/1e6):.1f} fps")
    print()
    print(f"=== EXPECTED PLAYBACK DURATION ===")
    print(f"  At 1.0x speed:   {total_sec:.1f} seconds")
    print(f"  At 0.5x speed:   {total_sec/0.5:.1f} seconds (slow mo)")
    print(f"  At 2.0x speed:   {total_sec/2.0:.1f} seconds (fast)")
    print(f"  At 5.0x speed:   {total_sec/5.0:.1f} seconds (fastest)")

    dup_count = sum(1 for f in ftimes if f < 1000)
    print(f"\n  Frames with frameTime < 1ms (dupes): {dup_count}")

    if frame > 0:
        print(f"\n  Expected FPS if frames from 30fps source: ~{int(30 * total_sec / frame)}")