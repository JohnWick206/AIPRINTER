"""纯标准库的 PNG 裁剪放大工具（不依赖 PIL）。"""
import sys, zlib, struct

def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not a png'
    pos = 8
    idat = b''
    w = h = bd = ct = None
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos+4])[0]
        typ = data[pos+4:pos+8]
        body = data[pos+8:pos+8+ln]
        if typ == b'IHDR':
            w, h, bd, ct, comp, filt, inter = struct.unpack('>IIBBBBB', body)
            assert bd == 8, f'bit depth {bd} not supported'
        elif typ == b'IDAT':
            idat += body
        elif typ == b'IEND':
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    stride = w * ch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        f = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos+stride]); pos += stride
        if f == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i-ch]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                b = prev[i]
                c = prev[i-ch] if i >= ch else 0
                p = a + b - c
                pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, ch, out

def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + bytes(rgb[y*w*3:(y+1)*w*3]) for y in range(h))
    def chunk(typ, body):
        return struct.pack('>I', len(body)) + typ + body + struct.pack('>I', zlib.crc32(typ + body) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    fx0, fy0, fx1, fy1 = [float(x) for x in sys.argv[3:7]]
    scale = int(sys.argv[7])

    w, h, ch, buf = read_png(src)
    print(f'原图 {w}x{h} channels={ch}')

    x0, x1 = int(w*fx0), int(w*fx1)
    y0, y1 = int(h*fy0), int(h*fy1)
    cw, chh = x1-x0, y1-y0
    print(f'裁剪 ({x0},{y0})-({x1},{y1}) = {cw}x{chh}，放大 {scale}x')

    ow, oh = cw*scale, chh*scale
    out = bytearray(ow*oh*3)
    for y in range(oh):
        sy = y0 + y//scale
        for x in range(ow):
            sx = x0 + x//scale
            si = (sy*w + sx)*ch
            di = (y*ow + x)*3
            out[di]   = buf[si]
            out[di+1] = buf[si+1] if ch >= 3 else buf[si]
            out[di+2] = buf[si+2] if ch >= 3 else buf[si]
    write_png(dst, ow, oh, out)
    print(f'已输出 {dst} ({ow}x{oh})')
