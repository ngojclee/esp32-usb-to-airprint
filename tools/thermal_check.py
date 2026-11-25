"""Decode an ESC/POS raster or TSPL stream from the thermal drivers into an
image, checking the structure on the way.

usage: python thermal_check.py <stream.prn> [out.png]
ESC/POS: ESC @, GS v 0 m xL xH yL yH <data> (1 = black) ..., ESC d n, GS V 66 n
TSPL:    SIZE/GAP/DIRECTION/REFERENCE/CLS, BITMAP x,y,wbytes,h,0,<data> (0 = black), PRINT
"""
import sys

data = open(sys.argv[1], "rb").read()


def fail(msg):
    sys.exit("FAIL: " + msg)


rows = []          # list of bytes, 1 = black
width_bytes = None

if data.startswith(b"\x1b@"):
    kind = "ESC/POS"
    pos = 2
    cut = False
    while pos < len(data):
        if data[pos:pos + 3] == b"\x1dv0":
            m, xl, xh, yl, yh = data[pos + 3:pos + 8]
            if m != 0:
                fail("GS v 0 mode %d" % m)
            wb, h = xl | xh << 8, yl | yh << 8
            if width_bytes not in (None, wb):
                fail("band width changed")
            width_bytes = wb
            pos += 8
            block = data[pos:pos + wb * h]
            if len(block) != wb * h:
                fail("truncated band")
            rows += [block[i * wb:(i + 1) * wb] for i in range(h)]
            pos += wb * h
        elif data[pos:pos + 2] == b"\x1bd":
            pos += 3
        elif data[pos:pos + 3] == b"\x1dVB":
            pos += 4
            cut = True
        else:
            fail("unknown byte 0x%02x at %d" % (data[pos], pos))
    if not cut:
        fail("no cut at the end")
elif data.startswith(b"SIZE"):
    kind = "TSPL"
    head_end = data.index(b"BITMAP ")
    head = data[:head_end].decode()
    for cmd in ("SIZE ", "GAP ", "CLS"):
        if cmd not in head:
            fail("missing " + cmd)
    comma = data.index(b",", head_end)
    fields = data[head_end + 7:].split(b",", 5)
    x, y, wb, h, mode = (int(f) for f in fields[:5])
    width_bytes = wb
    pos = head_end + 7 + sum(len(f) + 1 for f in fields[:5])
    bitmap = data[pos:pos + wb * h]
    if len(bitmap) != wb * h:
        fail("truncated bitmap")
    rows = [bytes(255 - b for b in bitmap[i * wb:(i + 1) * wb]) for i in range(h)]
    tail = data[pos + wb * h:]
    if b"PRINT 1,1" not in tail:
        fail("no PRINT after the bitmap")
    size = head.split("\r\n")[0]
    print(size)
else:
    fail("neither ESC/POS nor TSPL")

w, h = width_bytes * 8, len(rows)
ink = sum(bin(b).count("1") for r in rows for b in r)
print("%s: %d x %d dots, %.1f%% black" % (kind, w, h, 100.0 * ink / max(1, w * h)))
if not ink:
    fail("blank page")
if len(sys.argv) > 2:
    from PIL import Image
    img = Image.frombytes("1", (w, h), b"".join(rows)).point(lambda v: 255 - v)
    img.save(sys.argv[2])
    print("wrote", sys.argv[2])
print("OK")
