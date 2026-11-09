---
name: add-printer-driver
description: Add support for a new printer model or protocol family to this firmware (driver in components/printers, VID/PID matching, host selftest, README status table). Use when asked to support another printer, add a driver, or mark a model tested.
---

# Add a printer driver

Step-by-step for this repo. Read `docs/adding-a-printer.md` (user guide) and
the "Printer drivers" section of `AGENTS.md` (driver API) first. Then:

## 1. Classify the printer

- Get its USB VID:PID and IEEE 1284 device id (MFG/MDL strings): `lsusb -v`
  or Device Manager → Hardware Ids, or the board's serial log when the printer
  is plugged in ("unknown printer" line).
- Check its CUPS driver / PPD for the protocol: `MODE:SPL3` → Samsung/Dell
  QPDL; Canon PIXMA → canon_bjl variants; PCL/PostScript/PWG-raster-native →
  maybe passthrough.
- Decide the case:
  - **Same family, new model** → copy an existing driver entry, change ids.
  - **New protocol** → new file in `components/printers/`.

## 2. Write the driver

- Same family: e.g. another SPL laser = copy the `dell_b1160_driver` entry in
  `components/printers/samsung_qpdl.c`, set `usb_vid`/`usb_pid`/`usb_mdl`
  (and `usb_mfg` if needed), adjust `.resolutions` and `.papers` from the PPD.
- New protocol: implement `printer_driver_t` per `AGENTS.md` — job/page
  hooks, `printer_io_t` (`write`, `sync`, `progress` every 64 lines), paper
  table (mm/100, first entry = default), margins.
- **Never copy CUPS filter code** (SpliX, foo2zjs, gutenprint are GPL; this
  repo is MIT). Use them as format references only; write an independent
  encoder. PPD files are data and fine to mine.
- Register: `extern` + one line in `printer_drivers[]` in
  `components/printers/printers.c`, the file in
  `components/printers/CMakeLists.txt`, and `PRINTERS` in `host/Makefile`.

## 3. Test before/without the printer

- Host: `PRINTER_DRIVER=<id> host/airprint_host`, send a job, inspect
  `host/out/job-N.prn`.
- Encode→decode selftest: follow `host/qpdl_selftest.c` +
  `tools/qpdl_decode.py` — decode your stream and diff pixels against a
  reference capture (print-to-file through the vendor driver).
- `MODE=dev` on the board exercises the full path without printing.
- The BOOT button test page is the first real-hardware check.
- Don't flash hardware without the user's go-ahead (AGENTS.md working rules).

## 4. Ship it

- Add the model to the supported-printers table in **all three** READMEs
  (`README.md`, `README.vi.md`, `README.fr.md`): 🟡 untested unless a real
  print was seen (then 🟢 with what was tested).
- One firmware contains all drivers — no per-model build to update.
- Ask for/commit a test report via the "Printer test report" issue template.
