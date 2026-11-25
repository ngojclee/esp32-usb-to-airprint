# Driver roadmap

Which printer families this firmware drives, how far each one is verified,
and what comes next. The README tables show the same status per model.

## Status levels

| Level | Meaning | How a driver gets there |
|---|---|---|
| 🟢 **Verified** | printed correctly on the real printer | a maintainer or a [printer test report](https://github.com/ngojclee/esp32-usb-to-airprint/issues/new?template=printer-test-report.md) |
| 🔵 **Emulator-checked** | the stream is rendered by an *independent* program (GhostPCL, the vendor driver's capture, …) and matches the page | CI or a documented host check |
| 🟡 **Built, untested** | compiles, round-trips through our own decoder; never seen by a printer or an independent renderer | host selftest only |

A driver ships at 🟡 at the earliest. It never blocks other printers: a
wrong driver only prints garbage or nothing on its own models.

## Families

| Family | Driver | Printers | Status | Next step |
|---|---|---|---|---|
| Samsung / Dell SPL (QPDL v3) | `samsung_qpdl.c` | Dell B1160 | 🟢 | more models: their USB ids |
| Canon PIXMA (BJL) | `canon_pixma.c` | MG2550S | 🟢 | upstream; other models 🟡 |
| IPP-over-USB (driverless) | `ipp_usb.c` | any USB protocol-4 printer | 🟡 | a report from any HP / Epson / Brother owner |
| ESC/POS raster (receipt printers) | `thermal.c` | 58 / 80 mm thermal receipt printers | 🟡 | a report; check against a vendor capture |
| TSPL (label printers) | `thermal.c` | Xprinter, TSC, many 4" label printers | 🟡 | capture from the Xprinter Windows driver; real print |
| PCL raster (lasers) | `pcl.c` | HP LaserJet and most PCL lasers (Brother, Kyocera, Ricoh…) | 🟡 → 🔵 | GhostPCL rendering in CI |
| ESC/P-R (Epson inkjets) | — | Epson inkjets | planned | after PCL |

## How each family is tested without the printer

- **Our decoders** (`tools/*_decode.py`, `tools/*_check.py`) turn a driver's
  stream back into an image and compare it with the page that went in.
- **Independent renderers**: GhostPCL (Artifex) renders PCL to an image in
  CI. That catches misreadings of the spec that our own decoder would share.
- **Vendor captures**: a vendor's Windows driver installs without its
  printer; printing to a file port gives a reference stream to compare
  command layout with.
- **Matching**: when no USB id or model name matches, the firmware looks
  at the command sets in the printer's IEEE 1284 id (`CMD:`), so a printer
  that says `PCL`, `TSPL` or `ESC/POS` gets the generic driver for it.

What only a real printer shows: undocumented limits (the B1160 times out on
literal runs above 16 bytes although the format allows 128), USB pacing,
real margins and darkness. That is what 🟢 reports are for.
