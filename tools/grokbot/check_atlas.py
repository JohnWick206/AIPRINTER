#!/usr/bin/env python3
"""Decode a frame from grokbot.atlas to PNG, for sanity checking before flashing."""
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image

atlas = Path(sys.argv[1] if len(sys.argv) > 1 else "main/grokbot.atlas")
out = Path(sys.argv[2] if len(sys.argv) > 2 else "build/atlas_frame0.png")
clip_index = int(sys.argv[3]) if len(sys.argv) > 3 else 0
frame_index = int(sys.argv[4]) if len(sys.argv) > 4 else 0

d = atlas.read_bytes()
magic, ver, w, h, fps, n, res = struct.unpack("<4sHHHBBI", d[:16])
print(f"magic={magic} v{ver} {w}x{h} fps={fps} clips={n} size={len(d)}")

off = 16 + clip_index * 32
name, frames, start, coff, csize, packed = struct.unpack("<16sHHIII", d[off:off + 32])
name = name.split(b"\0")[0].decode("ascii", "replace")
print(f"clip[{clip_index}]={name} frames={frames} start={start} offset={coff} packed={packed}")
assert packed == w * h // 2, f"packed mismatch {packed} vs {w*h//2}"

frames_data = []
pos = 0
for i in range(frames):
    fo, lf = struct.unpack("<II", d[coff + i * 8: coff + i * 8 + 8])
    ln = lf & 0x7FFFFFFF
    delta = (lf >> 31) & 1
    foff = coff + fo
    flen, flags = struct.unpack("<IB", d[foff:foff + 5])
    payload = d[foff + 8: foff + 8 + flen]
    assert flen == ln, f"len mismatch frame {i}: {flen} vs {ln}"
    raw = zlib.decompress(payload)
    assert len(raw) == packed, f"inflate {len(raw)} vs {packed}"
    frames_data.append((raw, delta))
    if i == 0:
        cur = bytearray(raw)
    else:
        cur = bytearray(cur)
        if delta:
            for k in range(packed):
                cur[k] ^= raw[k]
        else:
            cur = bytearray(raw)
    if i == frame_index:
        break

img = Image.new("L", (w, h))
px = []
for b in cur:
    px.append((b >> 4) * 17)
    px.append((b & 15) * 17)
img.putdata(px[:w * h])
out.parent.mkdir(parents=True, exist_ok=True)
img.save(out)
print(f"wrote {out} (frame {frame_index}, delta={frames_data[frame_index][1]})")
lo, hi = img.getextrema()
print(f"luma range {lo}..{hi}")
