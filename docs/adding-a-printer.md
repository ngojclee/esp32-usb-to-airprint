# Adding a printer

How printer support works here, and how to add yours — three cases, from one
line to a new driver file.

## How detection works

`components/printers/printers.c` lists every known driver in
`printer_drivers[]`. When a printer is plugged in, `printer_driver_match()`
picks the driver by:

1. **USB vendor / product ids** (`usb_vid`, `usb_pid`), then
2. the **IEEE 1284 device ID** the printer reports (`usb_mdl` / `usb_mfg`,
   compared case-insensitively, ignoring a trailing " series").

If nothing matches, the build-time default `CONFIG_PRINTER_DRIVER` is used —
so a printer it doesn't know still gets *some* answer instead of silence.

## Case 1 — same protocol family, new model (one entry)

Example: another Samsung/Dell SPL laser (check `MODE:SPL3` in its 1284 id, or
capture a job and look for the QPDL band structure). Copy the
`dell_b1160_driver` entry in
[`samsung_qpdl.c`](../components/printers/samsung_qpdl.c), set its
`usb_vid`/`usb_pid`/`usb_mdl`, and adjust paper list / resolutions if the PPD
of its CUPS driver says otherwise. Same for a Canon PIXMA close to an existing
entry in `canon_pixma.c`.

Where to find the ids: `lsusb` on Linux, Device Manager on Windows
(*Hardware Ids*), or `system_profiler SPUSBDataType` on a Mac.

## Case 2 — a printer that already speaks a standard language

Many recent printers accept **PWG raster or URF** directly (they are
driverless/IPP-Everywhere capable even on USB). Those are handled by the
generic [`ipp_usb.c`](../components/printers/ipp_usb.c) driver: it wraps the
job in the HTTP-over-bulk framing of IPP-over-USB (USB protocol 4) and
re-encodes the pages as PWG raster. No per-model work is needed — the
firmware picks it automatically when a device shows a protocol-4 interface
and no other driver matches. The cheapest family to grow: if a printer needs
a related standard (e.g. a fixed header or a different attribute set), start
from that file.

## Case 3 — a new protocol family (new file)

Every vendor language is a file in `components/printers/` implementing
`printer_driver_t` (see `printer_driver.h`): paper list, resolutions, and the
function that turns the page raster into the printer's own stream.

- **CUPS drivers are documentation, not source.** Linux filters like SpliX
  (GPL-2.0), foo2zjs or gutenprint tell you the wire format — read them, don't
  copy them. GPL code cannot go into this MIT repo; write an independent
  implementation (the QPDL encoder here is one).
- **The PPD files are pure data** — paper sizes, margins, resolutions are fine
  to take straight from the driver's PPD.
- **Captures beat documentation.** Print a page through the vendor driver with
  a "print to file" port, and keep the capture as a test fixture.

## Testing without (much) hardware

- **`MODE=dev`** in `.env`: jobs are decoded and converted but nothing goes to
  the printer — good for checking the path end-to-end.
- **Host selftests**: `host/qpdl_selftest.c` builds a page on your PC and
  `tools/qpdl_decode.py` decodes the produced stream and compares it pixel by
  pixel with a reference capture. Write the same pair for a new protocol:
  encode → decode → diff. That verifies byte-level correctness before paper.
- **The BOOT button** prints the built-in test page — the fastest real check
  once a driver is flashed.

## Reporting a result

Whatever the outcome, open an
[issue](https://github.com/ngojclee/esp32-usb-to-airprint/issues) with the
printer model, the firmware version and what happened ("test page prints",
"garbled", "not detected, VID:PID = …"). Verified printers get 🟢 in the
README table; a PR adding the model line is welcome too.
