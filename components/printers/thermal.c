// Thermal printers at 203 dpi, monochrome:
//
//   escpos_driver  receipt printers (58 / 80 mm rolls), ESC/POS raster:
//                  ESC @, then "GS v 0" bit images in bands, a short feed and
//                  a cut (GS V 66). Trailing white lines are dropped, so a
//                  receipt is only as long as its content.
//   tspl_driver    label printers (TSC, Xprinter and other TSPL printers):
//                  SIZE / GAP / CLS, one BITMAP with the whole label, PRINT.
//                  In a TSPL bitmap a 0 bit prints, so the bits are inverted.
//
// Both keep the page as packed bits (1 = black) until page_end. Neither has
// been tried on a printer yet (README: untested); tools/thermal_check.py
// decodes both streams back into images.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mono.h"
#include "printer_driver.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define PAGE_MALLOC(n) heap_caps_malloc_prefer((n), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)
#else
#define PAGE_MALLOC(n) malloc(n)
#endif

#define DPI 203
#define MM_TO_DOTS(mm100) ((int)(((long)(mm100) * DPI + 1270) / 2540))     // 1/100 mm -> dots
#define BAND 128                // ESC/POS raster rows per GS v 0 command
#define MAX_PAGE_DOTS 24000     // ~3 m of receipt: refuse anything longer

typedef struct {
    int print_mm100;            // printable width
    int offset_mm100;           // from the paper's left edge to the printable area
} head_t;

// --- papers ----------------------------------------------------------------

static const printer_paper_t RECEIPT_PAPERS[] = {
    {"Receipt 80 mm", "om_receipt-80mm_80x297mm", 8000, 29700},
    {"Receipt 58 mm", "om_receipt-58mm_58x297mm", 5800, 29700},
};
static const head_t RECEIPT_HEADS[] = {{7200, 400}, {4800, 500}};

static const printer_paper_t LABEL_PAPERS[] = {
    {"Label 4x6 in", "na_index-4x6_4x6in", 10160, 15240},
    {"Label 100x150 mm", "om_label-100x150mm_100x150mm", 10000, 15000},
    {"Label 100x100 mm", "om_label-100x100mm_100x100mm", 10000, 10000},
    {"Label 100x50 mm", "om_label-100x50mm_100x50mm", 10000, 5000},
    {"Label 80x50 mm", "om_label-80x50mm_80x50mm", 8000, 5000},
    {"Label 60x40 mm", "om_label-60x40mm_60x40mm", 6000, 4000},
    {"Label 58x40 mm", "om_label-58x40mm_58x40mm", 5800, 4000},
    {"Label 50x30 mm", "om_label-50x30mm_50x30mm", 5000, 3000},
    {"Label 40x30 mm", "om_label-40x30mm_40x30mm", 4000, 3000},
};

// --- job -------------------------------------------------------------------

typedef struct {
    const printer_io_t *io;
    bool tspl;
    const printer_paper_t *paper;
    int dots_w, bytes_w;        // printable width
    int x0;                     // source pixels skipped on the left
    int rows;                   // rows of this page (TSPL: the label height)
    int src_w, bpp;
    int last_black;             // last row with ink, -1 = none
    uint8_t *bits;
    int err;
} job_t;

static int wr(job_t *j, const void *p, size_t n)
{
    if (!j->err && j->io->write(j->io->ctx, p, n) < 0)
        j->err = -1;
    return j->err;
}

static int wr_str(job_t *j, const char *s)
{
    return wr(j, s, strlen(s));
}

static void *job_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper)
{
    (void)dpi;
    job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->io = io;
    j->tspl = d->papers == LABEL_PAPERS;
    j->paper = &d->papers[paper];
    if (j->tspl) {
        j->dots_w = MM_TO_DOTS(j->paper->width);
        j->x0 = 0;
    } else {
        const head_t *hd = &RECEIPT_HEADS[paper];
        j->dots_w = MM_TO_DOTS(hd->print_mm100);
        j->x0 = MM_TO_DOTS(hd->offset_mm100);
        static const uint8_t INIT[] = {0x1b, '@'};
        wr(j, INIT, sizeof(INIT));
    }
    j->bytes_w = (j->dots_w + 7) / 8;
    if (j->err) {
        free(j);
        return NULL;
    }
    return j;
}

static int page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    job_t *j = job;
    j->src_w = src_w;
    j->bpp = bytes_pp;
    j->rows = j->tspl ? MM_TO_DOTS(j->paper->height) : src_h;
    if (j->rows <= 0 || j->rows > MAX_PAGE_DOTS)
        return 1;
    j->last_black = -1;
    j->bits = PAGE_MALLOC((size_t)j->bytes_w * j->rows);
    if (!j->bits)
        return 1;
    memset(j->bits, 0, (size_t)j->bytes_w * j->rows);
    return 0;
}

static int page_line(void *job, int y, const uint8_t *px)
{
    job_t *j = job;
    if (y >= j->rows)
        return 0;               // below the label: dropped
    if (mono_pack_line(px, j->src_w, j->bpp, y, j->x0, j->dots_w, j->bits + (size_t)y * j->bytes_w))
        j->last_black = y;
    return 0;
}

static void send_escpos(job_t *j)
{
    int rows = j->last_black + 1;
    for (int y0 = 0; y0 < rows && !j->err; y0 += BAND) {
        int n = rows - y0 < BAND ? rows - y0 : BAND;
        uint8_t h[8] = {0x1d, 'v', '0', 0, (uint8_t)(j->bytes_w & 0xff), (uint8_t)(j->bytes_w >> 8),
                        (uint8_t)(n & 0xff), (uint8_t)(n >> 8)};
        wr(j, h, sizeof(h));
        wr(j, j->bits + (size_t)y0 * j->bytes_w, (size_t)n * j->bytes_w);
    }
    // feed past the tear bar, then a partial cut with a little feed
    static const uint8_t END[] = {0x1b, 'd', 4, 0x1d, 'V', 66, 0};
    wr(j, END, sizeof(END));
}

static void send_tspl(job_t *j)
{
    char head[192];
    int w = j->paper->width, h = j->paper->height;
    snprintf(head, sizeof(head),
             "SIZE %d.%d mm,%d.%d mm\r\nGAP 2 mm,0 mm\r\nDIRECTION 1,0\r\nREFERENCE 0,0\r\nCLS\r\n"
             "BITMAP 0,0,%d,%d,0,",
             w / 100, (w % 100) / 10, h / 100, (h % 100) / 10, j->bytes_w, j->rows);
    wr_str(j, head);
    // 0 = a printed dot in TSPL: send the inverted bits, a line at a time
    uint8_t line[256];
    for (int y = 0; y < j->rows && !j->err; y++) {
        const uint8_t *src = j->bits + (size_t)y * j->bytes_w;
        for (int x = 0; x < j->bytes_w; x += (int)sizeof(line)) {
            int n = j->bytes_w - x < (int)sizeof(line) ? j->bytes_w - x : (int)sizeof(line);
            for (int i = 0; i < n; i++)
                line[i] = (uint8_t)~src[x + i];
            wr(j, line, (size_t)n);
        }
    }
    wr_str(j, "\r\nPRINT 1,1\r\n");
}

static int page_end(void *job)
{
    job_t *j = job;
    if (j->tspl)
        send_tspl(j);
    else
        send_escpos(j);
    free(j->bits);
    j->bits = NULL;
    return j->err ? 4 : 0;
}

static int job_end(void *job)
{
    job_t *j = job;
    int err = j->err;
    free(j->bits);
    free(j);
    return err;
}

// --- test page: a border, a 16-step grey ramp and a solid bar --------------

static int test_page(const printer_driver_t *d, const printer_io_t *io, int dpi, const char *label)
{
    (void)label;
    const printer_paper_t *pp = &d->papers[0];
    int w = MM_TO_DOTS(pp->width), h = d->papers == LABEL_PAPERS ? MM_TO_DOTS(pp->height) : MM_TO_DOTS(6000);
    void *job = job_begin(d, io, dpi, 0);
    if (!job)
        return -1;
    uint8_t *line = malloc((size_t)w);
    if (!line || page_begin(job, w, h, 1)) {
        free(line);
        job_end(job);
        return -1;
    }
    const int m = MM_TO_DOTS(600), t = MM_TO_DOTS(50);
    for (int y = 0; y < h; y++) {
        memset(line, 0xff, (size_t)w);
        if ((y >= m && y < m + t) || (y >= h - m - t && y < h - m))
            memset(line + m, 0, (size_t)(w - 2 * m));
        if (y >= m && y < h - m) {
            memset(line + m, 0, (size_t)t);
            memset(line + w - m - t, 0, (size_t)t);
        }
        if (y >= h * 3 / 10 && y < h * 5 / 10)
            for (int k = 0; k < 16; k++) {
                int x0 = m * 2 + (w - 4 * m) * k / 16, x1 = m * 2 + (w - 4 * m) * (k + 1) / 16;
                memset(line + x0, k * 17, (size_t)(x1 - x0));
            }
        if (y >= h * 6 / 10 && y < h * 7 / 10)
            memset(line + m * 2, 0, (size_t)(w - 4 * m));
        page_line(job, y, line);
        if (io->progress && (y & 63) == 63)
            io->progress(io->ctx, y, h);
    }
    free(line);
    page_end(job);
    return job_end(job);
}

// margin: the unprintable side border (receipt rolls: the head is narrower
// than the paper)
#define THERMAL_DRIVER(name_, id_, model_, papers_, margin_)                                                     \
    const printer_driver_t name_ = {                                                                             \
        .id = id_,                                                                                               \
        .make_model = model_,                                                                                    \
        .color = false,                                                                                          \
        .scanner = false,                                                                                        \
        .resolutions = {DPI},                                                                                    \
        .nresolutions = 1,                                                                                       \
        .papers = papers_,                                                                                       \
        .npapers = (int)(sizeof(papers_) / sizeof(papers_[0])),                                                  \
        .margin_left = margin_,                                                                                  \
        .margin_right = margin_,                                                                                 \
        .job_begin = job_begin,                                                                                  \
        .page_begin = page_begin,                                                                                \
        .page_line = page_line,                                                                                  \
        .page_end = page_end,                                                                                    \
        .job_end = job_end,                                                                                      \
        .test_page = test_page,                                                                                  \
    }

THERMAL_DRIVER(escpos_driver, "escpos", "ESC/POS receipt printer", RECEIPT_PAPERS, 500);
THERMAL_DRIVER(tspl_driver, "tspl", "TSPL label printer", LABEL_PAPERS, 0);
