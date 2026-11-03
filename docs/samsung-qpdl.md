# Samsung / Dell SPL (QPDL v3) — notes for the driver

What [`components/printers/samsung_qpdl.c`](../components/printers/samsung_qpdl.c) sends, and the printer behaviour it had to work around. Tested on a **Dell B1160** (USB `413c:5641`, IEEE 1284 `CMD:SPL,FWV,EXT; MODE:SPL3`), which is a Samsung ML-2160 series engine.

## Stream

```
PJL header          ESC%-12345X @PJL ... @PJL ENTER LANGUAGE = QPDL\n
per page:
  page header       17 bytes
  bands             one per 128 lines that has any black dot
  page footer       01 00 01
PJL tail            09 ESC%-12345X
```

### Page header (17 bytes, big-endian fields)

| Byte | Meaning | Value used |
|---|---|---|
| 0 | signature | `00` |
| 1 | vertical resolution / 100 | `0C` (1200 dpi) |
| 2–3 | copies | 1 |
| 4 | paper type | A4 2, Letter 0, Legal 1, Executive 3, Env #10 6, Monarch 7, C5 8, DL 9, JIS B5 11, ISO B5 12, A5 16, A6 17, C6 23, Folio 24, Index 3×5 27, Oficio 28, Statement 30 |
| 5–6 | printable width **in pixels** | e.g. 9920 for A4 @ 1200 dpi |
| 7–8 | printable height **in pixels** | page height minus the top hard margin |
| 9 | paper source | 1 (auto) |
| 10 | – | 0 |
| 11 | duplex | 1 (= simplex for v3) |
| 12 | tumble | 0 |
| 13 | – | 0 |
| 14 | QPDL version | 3 |
| 15 | colour planes | 1 |
| 16 | horizontal resolution / 100 | `0C` |

QPDL v1/v2 printers put the page size in 1/300 inch here; **v3 wants pixels**.

### Band

```
0C  band_no  width_px(BE16)  128(BE16)  11  size(BE32)
EF CD AB 09                     sub-header
<algorithm 0x11 data>
checksum (BE32) = 0x270 + sum of the data bytes    (0x270 = EF+CD+AB+09)
size = len(data) + 8
```

The band is **transposed** (byte column after byte column: the 128 rows of bytes 0, then of byte 1, …) and **inverted** (a 0 bit prints black). Empty bands are simply not sent.

The band origin is the printable corner: the left and top hard margins (12.5 pt ≈ 4.4 mm, the left one rounded up to a whole byte) are cut from the page raster, and the printer puts them back.

### Algorithm 0x11

A small LZ77 with a table of 64 back-reference distances, all little-endian:

```
u32       L, the number of leading bytes copied as is
64 × u16  distance table
L bytes   the leading bytes
tokens:
  0nnnnnnn  + n+1 bytes          literal run (1..128 bytes)
  1xxxxxxx  yyiiiiii             copy 3 + (yy:xxxxxxx) bytes (3..514)
                                 from table[i] bytes back
```

A copy may overlap its own output (distance 1 repeats a byte). The driver uses a fixed table: short distances (a pattern repeating down a byte column) and multiples of 128 ±1 (the same rows one or more columns to the left).

## Printer quirks (each one cost a debugging session)

1. **Long literal runs make the printer give up.** The format allows 128-byte literal runs, but with runs of 43 bytes and more (dithered grey) the B1160 prints *INTERNAL ERROR – Incomplete Session by time out* mid-page. The driver caps runs at **16 bytes**.
2. **A laser cannot pause.** Once the paper moves, the data must arrive at engine speed. The driver builds the whole page in PSRAM and sends it at `page_end`.
3. **ESP32-S3 USB host:** bulk OUT transfers longer than one DMA descriptor (4 KB) lose data (DWC2). `OUT_XFER_SIZE` is 4096.
4. **The end of the job must be flushed** before `usb_printer_job_end()`, which drops a part-filled transfer.
5. The *POSITION* printed on an error page is where the printer's parser was, which can be a band or two after the real problem (it reads ahead).

## Checking a stream on the computer

```bash
# MSVC (or gcc/clang with the same files)
cl /O2 /std:c11 /I components/airprint/include /I components/printers host/qpdl_selftest.c components/printers/samsung_qpdl.c
qpdl_selftest            # writes qpdl-out.prn + qpdl-expected.raw
python tools/qpdl_decode.py
# ... compared 256 rows, 0 pixel mismatches
```

`qpdl_decode.py` checks the page header, every band checksum and that each band decodes to exactly its size, and renders `qpdl-out.pgm`.
