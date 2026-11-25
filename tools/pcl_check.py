"""Decode the PCL 5 raster stream of the pcl driver into a page image, and
optionally compare it with GhostPCL's rendering of the same stream.

usage: python pcl_check.py <stream.prn> [out.png] [--gpcl6 path/to/gpcl6]

Our decoder understands the subset the driver writes (PJL, ESC E, &l#A/O/L/E,
*p#x#Y, *t#R, *r#S, *r1A, *b2M, *b#W PackBits rows, *b#Y, *rC, FF) and
places the raster 1/4 inch from the left edge, where PCL's portrait logical
page starts. With --gpcl6 the stream is also rendered by GhostPCL (an
independent PCL interpreter) and the two images must agree.
"""
import re
import subprocess
import sys
import tempfile

args = [a for a in sys.argv[1:] if not a.startswith("--")]
gpcl6 = sys.argv[sys.argv.index("--gpcl6") + 1] if "--gpcl6" in sys.argv else None
if gpcl6:
    args.remove(gpcl6)
path = args[0]
data = open(path, "rb").read()

# code: (width in, height in, metric) -- the logical page starts 71/300 in
# from the left on metric papers, 1/4 in on the others
SIZES = {26: (8.27, 11.69, True), 2: (8.5, 11, False), 3: (8.5, 14, False), 25: (5.83, 8.27, True),
         24: (4.13, 5.83, True), 1: (7.25, 10.5, False)}


def fail(msg):
    sys.exit("FAIL: " + msg)


if not data.startswith(b"\x1b%-12345X@PJL"):
    fail("no PJL header")
if b"@PJL ENTER LANGUAGE=PCL" not in data:
    fail("no ENTER LANGUAGE=PCL")
if not data.rstrip().endswith(b"\x1b%-12345X"):
    fail("no UEL at the end")

pcl = data[data.index(b"ENTER LANGUAGE=PCL\r\n") + 20:]
esc = re.compile(rb"\x1b([&*])([a-z])((?:-?\d+[a-z])*-?\d*[A-Z])")
pages, cur = [], None
pos = 0
res, width, mode, size = 300, 0, 0, 26
while pos < len(pcl):
    b = pcl[pos]
    if pcl[pos:pos + 2] == b"\x1bE":
        pos += 2
        continue
    if pcl[pos:pos + 9] == b"\x1b%-12345X":
        break
    if b == 0x0C:
        if cur is None:
            fail("form feed outside a page")
        pages.append(cur)
        cur = None
        pos += 1
        continue
    m = esc.match(pcl, pos)
    if not m:
        fail("unexpected byte 0x%02x at %d" % (b, pos))
    pos = m.end()
    group, ch, params = m.group(1).decode(), m.group(2).decode(), m.group(3).decode()
    for val, term in re.findall(r"(-?\d*)([a-zA-Z])", params):
        v = int(val) if val else 0
        key = (group + ch + term.upper()).encode()
        if key == b"&lA":
            size = v
        elif key == b"*tR":
            res = v
        elif key == b"*rS":
            width = v
        elif key == b"*rA":
            cur = {"res": res, "width": width, "size": size, "rows": []}
        elif key == b"*bM":
            mode = v
        elif key == b"*bY":
            cur["rows"] += [b""] * v
        elif key == b"*bW":
            raw = pcl[pos:pos + v]
            pos += v
            if mode != 2:
                fail("compression mode %d" % mode)
            row, i = bytearray(), 0
            while i < len(raw):
                n = raw[i]
                i += 1
                if n < 128:
                    row += raw[i:i + n + 1]
                    i += n + 1
                elif n > 128:
                    row += raw[i:i + 1] * (257 - n)
                    i += 1
            cur["rows"].append(bytes(row))
        # &lO &lL &lE *pX *pY *rC: layout only

if not pages:
    fail("no page")
from PIL import Image

pg = pages[0]
pw, ph, metric = SIZES[pg["size"]]
W, H = round(pw * pg["res"]), round(ph * pg["res"])
x0 = pg["res"] * 71 // 300 if metric else pg["res"] // 4
wb = (pg["width"] + 7) // 8
img = Image.new("1", (W, H), 1)
raster = b"".join(r[:wb].ljust(wb, b"\0") for r in pg["rows"])
r_img = Image.frombytes("1", (wb * 8, len(pg["rows"])), raster).point(lambda v: 255 - v)
img.paste(r_img.crop((0, 0, pg["width"], len(pg["rows"]))), (x0, 0))
ink = img.convert("L").histogram()[0]
print("%d page(s); page 1: size code %d, %d dpi, raster %d x %d, %.2f%% black"
      % (len(pages), pg["size"], pg["res"], pg["width"], len(pg["rows"]), 100.0 * ink / (W * H)))
if not ink:
    fail("blank page")
if args[1:]:
    img.save(args[1])
    print("wrote", args[1])

if gpcl6:
    with tempfile.TemporaryDirectory() as d:
        out = d + "/gpcl6.pbm"
        subprocess.run([gpcl6, "-dNOPAUSE", "-dBATCH", "-sDEVICE=pbmraw", "-r%d" % pg["res"], "-o", out, path],
                       check=True, capture_output=True)
        g = Image.open(out).convert("1")
        print("GhostPCL page: %d x %d" % g.size)
        if abs(g.size[0] - W) > 2 or abs(g.size[1] - H) > 2:
            fail("GhostPCL page size %s, expected %dx%d" % (g.size, W, H))
        # the last few columns of GhostPCL's bitmap are past the page edge
        box = (0, 0, W - 12, H)
        a, b = img.crop(box).tobytes(), g.crop(box).tobytes()
        diff = sum(bin(x ^ y).count("1") for x, y in zip(a, b))
        pct = 100.0 * diff / (W * H)
        print("GhostPCL vs decoder: %.3f%% of pixels differ" % pct)
        if pct > 0.01:
            g.save(args[1].replace(".png", "-gpcl6.png") if args[1:] else "gpcl6.png")
            fail("GhostPCL renders the stream differently")
print("OK")
