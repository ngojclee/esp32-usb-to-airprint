// Host self test for the Samsung QPDL (SPL3) driver.
//
// Feeds a deterministic page through dell_b1160_driver, writes the printer
// stream to qpdl-out.prn and the expected page rows to qpdl-expected.raw.
// tools/qpdl_decode.py reads both back and checks the round trip.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "printer_driver.h"

extern const printer_driver_t dell_b1160_driver;

#define W 4960                  // A4 at 600 dpi (PPD points, rounded to 8)
#define TOP 105                 // hard margin, from the PPD's 12.5 pt
#define ROWS 256                // two bands

static FILE *g_out;

static int sink(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    return fwrite(data, 1, len, g_out) == len ? 0 : -1;
}

static void pattern(uint8_t *line, int y)
{
    memset(line, 255, W);
    if (y < TOP + 32) {                     // solid bar
        for (int x = 200; x < 3000; x++)
            line[x] = 0;
        return;
    }
    if (y >= TOP + 96 && y < TOP + 160) {   // checkerboard
        for (int x = 200; x < 3000; x++)
            line[x] = (((x >> 3) ^ (y >> 3)) & 1) ? 0 : 255;
        return;
    }
    if (y >= TOP + 176 && y < TOP + 224) {  // horizontal gradient
        for (int x = 200; x < 3000; x++)
            line[x] = (uint8_t)(x * 255 / 3000);
        return;
    }
    // a few text-like strokes
    for (int x = 200; x < 3000; x++)
        line[x] = ((x % 37) < 3) ? 0 : 255;
}

int main(void)
{
    g_out = fopen("qpdl-out.prn", "wb");
    FILE *exp = fopen("qpdl-expected.raw", "wb");
    if (!g_out || !exp) {
        perror("fopen");
        return 1;
    }

    printer_io_t io = {.write = sink, .sync = NULL, .progress = NULL, .ctx = NULL};
    void *job = dell_b1160_driver.job_begin(&dell_b1160_driver, &io, 600, 0);
    if (!job) {
        fprintf(stderr, "job_begin failed\n");
        return 1;
    }
    if (dell_b1160_driver.page_begin(job, W, TOP + ROWS, 1) != 0) {
        fprintf(stderr, "page_begin failed\n");
        return 1;
    }

    uint8_t *line = malloc(W);
    for (int y = TOP; y < TOP + ROWS; y++) {
        pattern(line, y);
        const uint8_t *px = line;
        int white = 1;
        for (int x = 0; x < W; x++)
            if (line[x] != 255) {
                white = 0;
                break;
            }
        if (white)
            px = NULL;
        if (dell_b1160_driver.page_line(job, y, px) != 0) {
            fprintf(stderr, "page_line %d failed\n", y);
            return 1;
        }
        fwrite(line, 1, W, exp);
    }
    free(line);

    if (dell_b1160_driver.page_end(job) != 0) {
        fprintf(stderr, "page_end failed\n");
        return 1;
    }
    if (dell_b1160_driver.job_end(job) != 0) {
        fprintf(stderr, "job_end failed\n");
        return 1;
    }
    fclose(g_out);
    fclose(exp);

    printf("ok: qpdl-out.prn and qpdl-expected.raw written\n");
    return 0;
}
