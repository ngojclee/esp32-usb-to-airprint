"""Decode and validate a Samsung QPDL v3 (SPL3) stream.

Used by host/qpdl_selftest.c: decodes qpdl-out.prn, rebuilds the page raster
and compares it with qpdl-expected.raw. Also renders a PGM for eyeballing.

The format is described in docs/research-dell-b1160.md.
"""
import struct
import sys

BAND_HEIGHT = 128
PJL_START = b"\x1b%-12345X"
PJL_TAIL = b"\x09\x1b%-12345X"


def u16be(b, o):
    return (b[o] << 8) | b[o + 1]


def u32be(b, o):
    return struct.unpack_from(">I", b, o)[0]


def decode_0x11(data, size):
    """Samsung algorithm 0x11 -> raw bytes."""
    head = struct.unpack_from("<I", data, 0)[0]
    table = list(struct.unpack_from("<64H", data, 4))
    p = 4 + 128
    out = bytearray(data[p:p + head])
    p += head
    while len(out) < size:
        flag = data[p]
        if flag & 0x80:
            if p + 1 >= len(data):
                raise ValueError("truncated match token")
            b2 = data[p + 1]
            x = (flag & 0x7F) | (((b2 & 0xC0) >> 6) << 7)
            length = x + 3
            dist = table[b2 & 0x3F]
            p += 2
            if dist == 0 or dist > len(out):
                raise ValueError("bad back reference")
            for _ in range(length):
                out.append(out[-dist])
        else:
            count = (flag & 0x7F) + 1
            out += data[p + 1:p + 1 + count]
            p += 1 + count
        if len(out) > size:
            raise ValueError("decoder overran the band")
    return bytes(out[:size]), p


def parse(blob):
    if not blob.startswith(PJL_START):
        raise ValueError("no PJL header")
    p = blob.index(b"@PJL ENTER LANGUAGE = QPDL\n") + len(b"@PJL ENTER LANGUAGE = QPDL\n")
    hdr = blob[p:p + 17]
    if len(hdr) < 17 or hdr[0] != 0:
        raise ValueError("bad page header")
    page = {
        "yres": hdr[1] * 100,
        "copies": u16be(hdr, 2),
        "paper": hdr[4],
        "w300": u16be(hdr, 5),
        "h300": u16be(hdr, 7),
        "source": hdr[9],
        "duplex": hdr[11],
        "qpdl": hdr[14],
        "planes": hdr[15],
        "xres": hdr[16] * 100,
    }
    p += 17

    bands = {}
    while p < len(blob):
        if blob[p] == 1:                       # page footer
            break
        if blob[p] != 0x0C:
            raise ValueError("unexpected byte 0x%02x at %d" % (blob[p], p))
        band = blob[p + 1]
        width = u16be(blob, p + 2)
        height = u16be(blob, p + 4)
        comp = blob[p + 6]
        size = u32be(blob, p + 7)
        p += 11
        if blob[p:p + 4] != b"\xef\xcd\xab\x09":
            raise ValueError("bad band signature")
        p += 4
        payload = blob[p:p + size - 8]
        p += size - 8
        checksum = u32be(blob, p)
        p += 4
        want = (sum(payload) + 0xEF + 0xCD + 0xAB + 0x09) & 0xFFFFFFFF
        if checksum != want:
            raise ValueError("band %d checksum %08x != %08x" % (band, checksum, want))
        if comp != 0x11:
            raise ValueError("unexpected compression 0x%02x" % comp)
        if height != BAND_HEIGHT or width % 8:
            raise ValueError("unexpected band geometry %dx%d" % (width, height))
        raw, used = decode_0x11(payload, width // 8 * BAND_HEIGHT)
        if used != len(payload):
            print("warning: band %d used %d of %d bytes" % (band, used, len(payload)))
        bands[band] = (width, raw)
    return page, bands


def main():
    blob = open("qpdl-out.prn", "rb").read()
    page, bands = parse(blob)
    print("page:", page)
    print("bands:", sorted(bands))

    width_px, raw = bands[sorted(bands)[0]]
    line_bytes = width_px // 8

    # reconstruct: band buffer is column-major and inverted
    rows = {}
    for band, (w, data) in bands.items():
        lb = w // 8
        out = bytearray(lb * BAND_HEIGHT)
        for i in range(len(data)):
            out[i] = (~data[i]) & 0xFF
        for y in range(BAND_HEIGHT):
            rows[band * BAND_HEIGHT + y] = bytes(
                out[x * BAND_HEIGHT + y] for x in range(lb)
            )

    expected = open("qpdl-expected.raw", "rb").read()
    exp_rows = len(expected) // width_px

    # The band starts at the printable corner: the driver drops the left hard
    # margin (12.5 pt, rounded up to a byte) from the page raster.
    dpi = page["xres"]
    hard_x = (25 * dpi + 144 - 1) // 144
    hard_x = (hard_x + 7) & ~7

    bad = 0
    bayer = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))
    for i in range(exp_rows):
        want = expected[i * width_px:(i + 1) * width_px]
        got = rows.get(i, bytes(line_bytes))
        page_row = 105 + i
        for p in range(width_px):
            col = p - hard_x
            # the driver's rule: near-black and near-white stay solid,
            # midtones get the 4x4 ordered dither
            v = want[p]
            if v < 48:
                exp_bit = 1
            elif v > 208:
                exp_bit = 0
            else:
                exp_bit = 1 if v < bayer[page_row & 3][p & 3] * 16 + 8 else 0
            bit = 0
            if 0 <= col < width_px:
                b = got[col >> 3] if (col >> 3) < len(got) else 0
                bit = 1 if b & (0x80 >> (col & 7)) else 0
            if bit != exp_bit:
                if bad < 10:
                    print("mismatch row %d x %d" % (i, p))
                bad += 1
    print("compared %d rows, %d pixel mismatches (hard_x=%d)" % (exp_rows, bad, hard_x))

    # eyeball render, in page coordinates
    with open("qpdl-out.pgm", "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (width_px, exp_rows))
        for i in range(exp_rows):
            got = rows.get(i, bytes(line_bytes))
            line = bytearray(width_px)
            for p in range(width_px):
                col = p - hard_x
                b = 0
                if 0 <= col < width_px:
                    b = got[col >> 3] if (col >> 3) < len(got) else 0
                line[p] = 0 if b & (0x80 >> (col & 7)) else 255
            f.write(line)

    return 0 if bad == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
