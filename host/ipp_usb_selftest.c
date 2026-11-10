// Host check for the ipp-usb driver: runs a small job through the driver's
// job/page API into a file, then tools/ipp_usb_check.py verifies the
// HTTP+chunked+IPP+PWG structure and decodes the raster pixel by pixel.
// Build (MSVC):  cl /I..\components\airprint\include /I..\components\airprint
//                ipp_usb_selftest.c ..\components\printers\ipp_usb.c
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "printer_driver.h"

extern const printer_driver_t ipp_usb_driver;

static FILE *out;
static int wr(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    return fwrite(data, 1, len, out) == len ? 0 : -1;
}
static int sync_cb(void *ctx, const char *what)
{
    (void)ctx;
    fprintf(stderr, "sync: %s\n", what);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "ipp_usb.prn";
    out = fopen(path, "wb");
    if (!out)
        return 1;

    const printer_driver_t *d = &ipp_usb_driver;
    printer_io_t io = {.write = wr, .sync = sync_cb};
    const int dpi = 300, paper = 0;     // A4
    const int w = 2500, h = 350;        // small page, 300 dpi ~ 85 x 12 mm

    void *job = d->job_begin(d, &io, dpi, paper);
    if (!job || d->page_begin(job, w, h, 1)) {
        fprintf(stderr, "job/page begin failed\n");
        return 2;
    }
    uint8_t *line = malloc(w);
    for (int y = 0; y < h; y++) {
        memset(line, 0xff, w);
        if (y < 47 || y >= h - 47)                      // ~4 mm border
            memset(line, 0, w);
        if (y >= 47 && y < h - 47) {
            memset(line, 0, 12);
            memset(line + w - 12, 0, 12);
        }
        if (y >= 100 && y < 150)                        // grey ramp
            for (int k = 0; k < 16; k++)
                memset(line + 100 + (w - 200) * k / 16, k * 17, (w - 200) / 16);
        if (y >= 200 && y < 220)                        // solid bar
            memset(line + 100, 0, w - 200);
        if (d->page_line(job, y, line))
            break;
    }
    free(line);
    int e = d->page_end(job) | d->job_end(job);
    fclose(out);
    printf("%s: %s\n", path, e ? "FAILED" : "written");
    return e ? 3 : 0;
}
