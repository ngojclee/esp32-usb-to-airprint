// Generic PCL 5 raster driver for monochrome laser printers (HP LaserJet and
// the many lasers that speak PCL: Brother, Kyocera, Ricoh, Lexmark...).
//
//   PJL: UEL, JOB, SET RESOLUTION, ENTER LANGUAGE=PCL
//   per page: ESC E, page size (ESC &l#A), portrait, no top margin
//             (ESC &l0E: row 0 = top of the logical page), raster at the
//             job's resolution: ESC *t#R, ESC *r#S, ESC *r1A,
//             rows as TIFF PackBits (ESC *b2M, ESC *b#W), white runs as
//             ESC *b#Y, ESC *rC, form feed
//   PJL: ESC E, UEL, EOJ, UEL
//
// The portrait logical page starts 1/4 inch from the left paper edge on inch
// papers and 71/300 inch on metric ones (A4: 142 dots at 600 dpi, as GhostPCL
// renders it), so that much of the source raster is skipped on the left.
// No USB ids: chosen for a printer whose IEEE 1284 command set lists PCL
// and that no model driver matched (printers.c). CI renders the stream with
// GhostPCL (tools/pcl_check.py); not tried on a printer yet.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mono.h"
#include "printer_driver.h"

typedef struct {
    int code;                   // ESC &l#A page size
    bool metric;                // logical page 71/300 in from the left (A4 & co), else 1/4 in
} pcl_size_t;

static const printer_paper_t PAPERS[] = {
    {"A4", "iso_a4_210x297mm", 21000, 29700},
    {"Letter", "na_letter_8.5x11in", 21590, 27940},
    {"Legal", "na_legal_8.5x14in", 21590, 35560},
    {"A5", "iso_a5_148x210mm", 14800, 21000},
    {"A6", "iso_a6_105x148mm", 10500, 14800},
    {"Executive", "na_executive_7.25x10.5in", 18415, 26670},
    {"B5 (JIS)", "jis_b5_182x257mm", 18200, 25700},
    {"B5 (ISO)", "iso_b5_176x250mm", 17600, 25000},
    {"Envelope #10", "na_number-10_4.125x9.5in", 10478, 24130},
    {"Envelope DL", "iso_dl_110x220mm", 11000, 22000},
    {"Envelope C5", "iso_c5_162x229mm", 16200, 22900},
    {"Envelope Monarch", "na_monarch_3.875x7.5in", 9843, 19050},
};
static const pcl_size_t SIZES[] = {{26, true}, {2, false}, {3, false}, {25, true},   {24, true},  {1, false},
                                   {45, true}, {100, true}, {81, false}, {90, true}, {91, true}, {80, false}};
_Static_assert(sizeof(PAPERS) / sizeof(PAPERS[0]) == sizeof(SIZES) / sizeof(SIZES[0]), "one code per paper");

typedef struct {
    const printer_io_t *io;
    int dpi, size_code;
    int x0;                     // source pixels left of the logical page
    int src_w, bpp, w, bytes_w;
    uint8_t *row, *packed;
    int white_rows;             // pending ESC *b#Y
    int err;
} job_t;

static int wr(job_t *j, const void *p, size_t n)
{
    if (!j->err && j->io->write(j->io->ctx, p, n) < 0)
        j->err = -1;
    return j->err;
}

static int wrs(job_t *j, const char *s)
{
    return wr(j, s, strlen(s));
}

static int wrf(job_t *j, const char *fmt, int v)
{
    char b[48];
    int n = snprintf(b, sizeof(b), fmt, v);
    return wr(j, b, (size_t)n);
}

// TIFF PackBits: n >= 0 then n + 1 literal bytes; n in -1..-127 then one
// byte repeated 1 - n times
static size_t packbits(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t i = 0, o = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 128 && in[i + run] == in[i])
            run++;
        if (run >= 2) {
            out[o++] = (uint8_t)(1 - (int)run);
            out[o++] = in[i];
            i += run;
            continue;
        }
        size_t start = i, lit = 0;
        while (i < n && lit < 128) {
            if (i + 1 < n && in[i + 1] == in[i])
                break;
            i++;
            lit++;
        }
        out[o++] = (uint8_t)(lit - 1);
        memcpy(out + o, in + start, lit);
        o += lit;
    }
    return o;
}

static void *job_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper)
{
    job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->io = io;
    j->dpi = dpi;
    j->size_code = SIZES[paper].code;
    j->x0 = SIZES[paper].metric ? dpi * 71 / 300 : dpi / 4;
    char pjl[160];
    int n = snprintf(pjl, sizeof(pjl),
                     "\x1b%%-12345X@PJL JOB NAME=\"print\"\r\n@PJL SET RESOLUTION=%d\r\n"
                     "@PJL ENTER LANGUAGE=PCL\r\n",
                     dpi);
    wr(j, pjl, (size_t)n);
    (void)d;
    if (j->err) {
        free(j);
        return NULL;
    }
    return j;
}

static int page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    job_t *j = job;
    (void)src_h;
    j->src_w = src_w;
    j->bpp = bytes_pp;
    j->w = src_w - 2 * j->x0;
    if (j->w <= 0)
        return 1;
    j->bytes_w = (j->w + 7) / 8;
    j->row = malloc((size_t)j->bytes_w);
    j->packed = malloc((size_t)j->bytes_w + j->bytes_w / 128 + 2);
    if (!j->row || !j->packed)
        return 1;
    j->white_rows = 0;
    wrs(j, "\x1b" "E");
    wrf(j, "\x1b&l%dA", j->size_code);
    wrs(j, "\x1b&l0O\x1b&l0L\x1b&l0E\x1b*p0x0Y");
    wrf(j, "\x1b*t%dR", j->dpi);
    wrf(j, "\x1b*r%dS", j->w);
    wrs(j, "\x1b*r1A\x1b*b2M");
    return j->err ? 1 : 0;
}

static int page_line(void *job, int y, const uint8_t *px)
{
    job_t *j = job;
    if (!mono_pack_line(px, j->src_w, j->bpp, y, j->x0, j->w, j->row)) {
        j->white_rows++;
        return 0;
    }
    if (j->white_rows) {
        wrf(j, "\x1b*b%dY", j->white_rows);
        j->white_rows = 0;
    }
    size_t n = (size_t)j->bytes_w;
    while (n && !j->row[n - 1])
        n--;                    // the printer fills a short row with white
    size_t p = packbits(j->row, n, j->packed);
    wrf(j, "\x1b*b%dW", (int)p);
    wr(j, j->packed, p);
    return j->err ? 4 : 0;
}

static int page_end(void *job)
{
    job_t *j = job;
    wrs(j, "\x1b*rC\x0c");
    free(j->row);
    free(j->packed);
    j->row = j->packed = NULL;
    return j->err ? 4 : 0;
}

static int job_end(void *job)
{
    job_t *j = job;
    wrs(j, "\x1b" "E\x1b%-12345X@PJL EOJ\r\n\x1b%-12345X");
    int err = j->err;
    free(j->row);
    free(j->packed);
    free(j);
    return err;
}

// --- test page: border 10 mm, 16-step grey ramp, solid bar ------------------

static int test_page(const printer_driver_t *d, const printer_io_t *io, int dpi, const char *label)
{
    (void)label;
    const printer_paper_t *pp = &d->papers[0];
    const double mm = dpi / 25.4;
    int w = (int)(pp->width / 100.0 * mm), h = (int)(pp->height / 100.0 * mm);
    void *job = job_begin(d, io, dpi, 0);
    if (!job)
        return -1;
    uint8_t *line = malloc((size_t)w);
    if (!line || page_begin(job, w, h, 1)) {
        free(line);
        job_end(job);
        return -1;
    }
    const int m = (int)(10 * mm), t = (int)(0.5 * mm);
    for (int y = 0; y < h; y++) {
        memset(line, 0xff, (size_t)w);
        if ((y >= m && y < m + t) || (y >= h - m - t && y < h - m))
            memset(line + m, 0, (size_t)(w - 2 * m));
        if (y >= m && y < h - m) {
            memset(line + m, 0, (size_t)t);
            memset(line + w - m - t, 0, (size_t)t);
        }
        if (y >= (int)(40 * mm) && y < (int)(55 * mm))
            for (int k = 0; k < 16; k++) {
                int x0 = 2 * m + (w - 4 * m) * k / 16, x1 = 2 * m + (w - 4 * m) * (k + 1) / 16;
                memset(line + x0, k * 17, (size_t)(x1 - x0));
            }
        if (y >= (int)(65 * mm) && y < (int)(70 * mm))
            memset(line + 2 * m, 0, (size_t)(w - 4 * m));
        page_line(job, y, line);
        if (io->progress && (y & 63) == 63)
            io->progress(io->ctx, y, h);
    }
    free(line);
    page_end(job);
    return job_end(job);
}

const printer_driver_t pcl_driver = {
    .id = "pcl",
    .make_model = "Generic PCL laser printer",
    .color = false,
    .scanner = false,
    .resolutions = {300, 600},
    .nresolutions = 2,
    .papers = PAPERS,
    .npapers = (int)(sizeof(PAPERS) / sizeof(PAPERS[0])),
    .margin_left = 635,
    .margin_right = 635,
    .margin_top = 420,
    .margin_bottom = 420,
    .job_begin = job_begin,
    .page_begin = page_begin,
    .page_line = page_line,
    .page_end = page_end,
    .job_end = job_end,
    .test_page = test_page,
};
