"""Verify an ipp-usb driver stream: HTTP head, chunked body, IPP Print-Job,
then decode the PWG raster pages and report geometry + ink coverage.

usage: python ipp_usb_check.py <file.prn>
"""
import struct
import sys

data = open(sys.argv[1], "rb").read()

# --- HTTP head -------------------------------------------------------------
he = data.index(b"\r\n\r\n")
head = data[:he].decode()
assert head.startswith("POST /ipp/print HTTP/1.1"), "bad request line"
assert "application/ipp" in head and "chunked" in head, head
print("HTTP head ok: POST /ipp/print, chunked")

# --- dechunk ---------------------------------------------------------------
pos = he + 4
body = bytearray()
while True:
    e = data.index(b"\r\n", pos)
    n = int(data[pos:e], 16)
    pos = e + 2
    if n == 0:
        break
    body += data[pos:pos + n]
    pos += n + 2
print("chunked body: %d bytes" % len(body))

# --- IPP message -----------------------------------------------------------
assert body[0:2] == b"\x02\x00", "IPP version"
op, req = struct.unpack(">HI", body[2:8])
assert op == 0x0002, "expected Print-Job"
print("IPP Print-Job, request-id %d" % req)
pos = 8
seen = {}
while body[pos] != 0x03:
    assert body[pos] in (1, 2), "bad attribute group 0x%02x" % body[pos]
    group = body[pos]
    pos += 1
    while body[pos] > 0x0f:                     # value tag
        tag = body[pos]
        nl = struct.unpack(">H", body[pos + 1:pos + 3])[0]
        name = body[pos + 3:pos + 3 + nl].decode()
        vl = struct.unpack(">H", body[pos + 3 + nl:pos + 5 + nl])[0]
        val = body[pos + 5 + nl:pos + 5 + nl + vl]
        seen[name] = val
        pos += 5 + nl + vl
pos += 1
for k in ("attributes-charset", "printer-uri", "document-format", "media"):
    assert k in seen, "missing " + k
assert seen["document-format"] == b"image/pwg-raster"
print("IPP attrs ok: media=%s" % seen["media"].decode())

# --- PWG raster ------------------------------------------------------------
assert body[pos:pos + 4] == b"RaS2", "missing raster magic"
pos += 4
page = 0
ink = 0
while pos < len(body):
    hdr = body[pos:pos + 1796]
    if len(hdr) < 1796:
        break
    pos += 1796
    page += 1
    dpi = struct.unpack(">I", hdr[276:280])[0]
    w, h = struct.unpack(">II", hdr[372:380])
    bits, bpl = struct.unpack(">II", hdr[388:396])
    cspace = struct.unpack(">I", hdr[400:404])[0]
    bpp = bits // 8
    assert bpp in (1, 3) and cspace in (18, 19), "unsupported page"
    rows, y = bytearray(), 0
    line = b""
    while y < h:
        rep = body[pos]
        pos += 1
        x = 0
        row = bytearray()
        while x < bpl:
            n = body[pos]
            pos += 1
            if n == 128:
                row += b"\xff" * (bpl - x)
                x = bpl
            elif n < 128:
                row += body[pos:pos + bpp] * (n + 1)
                pos += bpp
                x += (n + 1) * bpp
            else:
                c = 257 - n
                row += body[pos:pos + c * bpp]
                pos += c * bpp
                x += c * bpp
        assert len(row) == bpl
        ink += sum(bpl - b for b in row)          # 255=white
        y += rep + 1
    assert y == h, "page %d: got %d rows, want %d" % (page, y, h)
    print("page %d: %dx%d px, %d dpi, %d bpp" % (page, w, h, dpi, bits))

assert page > 0, "no pages"
assert ink > 0, "blank page (no ink)"
print("OK: %d page(s), %d ink units" % (page, ink))
