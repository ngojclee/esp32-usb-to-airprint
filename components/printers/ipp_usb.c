// IPP-over-USB printers (USB class 7 subclass 1 protocol 4): the bulk pipe
// carries plain HTTP/1.1, so a "driver" is a Print-Job request whose document
// is re-encoded to PWG raster ("RaS2" pages). This is the driverless USB
// path used by ippusbxd on Linux; many recent HP/Epson/Brother printers
// expose it alongside or instead of a vendor protocol.
//
// The driver has no USB ids or model names: it is selected only as the
// fallback for a device whose descriptor shows a protocol-4 interface
// (print_job.c). Stream layout sent over bulk-OUT:
//
//   POST /ipp/print HTTP/1.1  +  Transfer-Encoding: chunked
//   chunk: IPP Print-Job (version 2.0, media + print-resolution)
//   chunk(s): "RaS2" + 1796-byte page header + RLE rows, per page
//   final chunk: 0
//
// Row compression mirrors components/airprint/raster.c:
//   repeat byte r: the row is used r + 1 times
//   n < 128: the next pixel is repeated n + 1 times
//   n = 128: the rest of the line is white
//   n > 128: 257 - n literal pixels follow

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "printer_driver.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
// per-line working buffers: internal RAM (PSRAM is several times slower)
#define IPP_MALLOC(n) heap_caps_malloc((n), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#else
#define IPP_MALLOC(n) malloc(n)
#endif

#define CHUNK_SIZE 8192         // bytes per HTTP chunk (flush boundary)
#define PWG_HEADER_SIZE 1796

// ---------------------------------------------------------------- papers ---

static const printer_paper_t PAPERS[] = {
    {"A4", "iso_a4_210x297mm", 21000, 29700},
    {"Letter", "na_letter_8.5x11in", 21590, 27940},
    {"A5", "iso_a5_148x210mm", 14800, 21000},
    {"A6", "iso_a6_105x148mm", 10500, 14800},
    {"B5 (JIS)", "jis_b5_182x257mm", 18200, 25700},
    {"B5 (ISO)", "iso_b5_176x250mm", 17600, 25000},
    {"Legal", "na_legal_8.5x14in", 21590, 35560},
    {"Executive", "na_executive_7.25x10.5in", 18415, 26670},
    {"Folio", "na_foolscap_8.5x13in", 21590, 33020},
    {"Oficio", "na_oficio_8.5x13.4in", 21590, 34036},
    {"Statement", "na_invoice_5.5x8.5in", 13970, 21590},
    {"Envelope #10", "na_number-10_4.125x9.5in", 10478, 24130},
    {"Envelope DL", "iso_dl_110x220mm", 11000, 22000},
    {"Envelope C5", "iso_c5_162x229mm", 16200, 22900},
    {"Envelope C6", "iso_c6_114x162mm", 11400, 16200},
    {"Envelope Monarch", "na_monarch_3.875x7.5in", 9843, 19050},
    {"Index card 3x5", "na_index-3x5_3x5in", 7620, 12700},
};
#define NPAPERS ((int)(sizeof(PAPERS) / sizeof(PAPERS[0])))

// ------------------------------------------------------------------ job ----

typedef struct {
    const printer_io_t *io;
    const printer_paper_t *paper;
    int dpi;
    int err;

    // per page
    int w, h, bpp;
    uint8_t *row, *prev;    // encoded rows, for repeat detection
    size_t rlen, plen;
    int rep;                // repeat count pending for prev
    int page_lines;

    uint8_t *chunk;         // HTTP chunk being filled
    size_t clen;
} job_t;

static int wr(job_t *j, const void *data, size_t len)
{
    if (!j->err && j->io->write(j->io->ctx, data, len) < 0)
        j->err = -1;
    return j->err;
}

// Flush the current HTTP chunk: "<hex>\r\n<data>\r\n".
static int chunk_flush(job_t *j)
{
    if (!j->clen)
        return j->err;
    char head[16];
    int n = snprintf(head, sizeof(head), "%zx\r\n", j->clen);
    if (wr(j, head, n) || wr(j, j->chunk, j->clen) || wr(j, "\r\n", 2))
        return j->err;
    j->clen = 0;
    return 0;
}

// Append body bytes, emitting full chunks.
static int chunk_out(job_t *j, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len) {
        size_t take = CHUNK_SIZE - j->clen;
        if (take > len)
            take = len;
        memcpy(j->chunk + j->clen, p, take);
        j->clen += take;
        p += take;
        len -= take;
        if (j->clen == CHUNK_SIZE && chunk_flush(j))
            return j->err;
    }
    return 0;
}

// ------------------------------------------------------------- IPP bits ---

static void be16(uint8_t **p, uint16_t v)
{
    *(*p)++ = (uint8_t)(v >> 8);
    *(*p)++ = (uint8_t)v;
}

static void be32(uint8_t **p, uint32_t v)
{
    *(*p)++ = (uint8_t)(v >> 24);
    *(*p)++ = (uint8_t)(v >> 16);
    *(*p)++ = (uint8_t)(v >> 8);
    *(*p)++ = (uint8_t)v;
}

static void attr_str(uint8_t **p, uint8_t tag, const char *name, const char *val)
{
    size_t nl = strlen(name), vl = strlen(val);
    *(*p)++ = tag;
    be16(p, (uint16_t)nl);
    memcpy(*p, name, nl);
    *p += nl;
    be16(p, (uint16_t)vl);
    memcpy(*p, val, vl);
    *p += vl;
}

static void attr_res(uint8_t **p, const char *name, int dpi)
{
    size_t nl = strlen(name);
    *(*p)++ = 0x32;     // IPP 'resolution'
    be16(p, (uint16_t)nl);
    memcpy(*p, name, nl);
    *p += nl;
    be16(p, 9);
    be32(p, (uint32_t)dpi);
    be32(p, (uint32_t)dpi);
    *(*p)++ = 3;        // dots per inch
}

// ------------------------------------------------------------- job flow ---

static void *job_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper)
{
    job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->io = io;
    j->dpi = dpi;
    j->paper = &d->papers[paper];
    j->chunk = IPP_MALLOC(CHUNK_SIZE);
    if (!j->chunk) {
        free(j);
        return NULL;
    }

    static const char HEAD[] =
        "POST /ipp/print HTTP/1.1\r\n"
        "Host: printer\r\n"
        "Content-Type: application/ipp\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    wr(j, HEAD, sizeof(HEAD) - 1);

    // IPP Print-Job, version 2.0
    uint8_t msg[512], *p = msg;
    *p++ = 2;
    *p++ = 0;
    be16(&p, 0x0002);                   // Print-Job
    be32(&p, 1);                        // request-id
    *p++ = 0x01;                        // operation-attributes-tag
    attr_str(&p, 0x47, "attributes-charset", "utf-8");
    attr_str(&p, 0x48, "attributes-natural-language", "en");
    attr_str(&p, 0x45, "printer-uri", "ipp://printer/ipp/print");
    attr_str(&p, 0x42, "requesting-user-name", "esp32");
    attr_str(&p, 0x42, "job-name", "Print job");
    attr_str(&p, 0x49, "document-format", "image/pwg-raster");
    *p++ = 0x02;                        // job-attributes-tag
    attr_str(&p, 0x44, "media", j->paper->pwg);
    attr_res(&p, "print-resolution", dpi);
    *p++ = 0x03;                        // end-of-attributes-tag
    chunk_out(j, msg, p - msg);
    if (j->err) {
        free(j);
        return NULL;
    }
    return j;
}

static void pwg_header(job_t *j, uint8_t *h)
{
    memset(h, 0, PWG_HEADER_SIZE);
    memcpy(h, "PwgRaster", 9);          // MediaClass
    uint8_t *p;
    p = h + 276;
    be32(&p, j->dpi);
    be32(&p, j->dpi);                   // HWResolution
    p = h + 340;
    be32(&p, 1);                        // NumCopies
    p = h + 352;
    be32(&p, j->paper->width * 72 / 2540);   // PageSize, points
    be32(&p, j->paper->height * 72 / 2540);
    p = h + 372;
    be32(&p, j->w);
    be32(&p, j->h);                     // cupsWidth/Height
    p = h + 384;
    be32(&p, 8);                        // bits per colour
    be32(&p, j->bpp * 8);               // bits per pixel
    be32(&p, j->w * j->bpp);            // bytes per line
    be32(&p, 0);                        // chunked order
    p = h + 400;
    be32(&p, j->bpp == 3 ? 19 : 18);    // srgb / sgray
    p = h + 420;
    be32(&p, j->bpp);                   // colours
}

static int page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    job_t *j = job;
    j->w = src_w;
    j->h = src_h;
    j->bpp = bytes_pp == 3 ? 3 : 1;
    j->rep = 0;
    j->plen = 0;
    j->page_lines = 0;

    size_t row_max = (size_t)src_w * j->bpp * 2 + 16;  // worst: ~1 byte per px
    j->row = IPP_MALLOC(row_max);
    j->prev = IPP_MALLOC(row_max);
    if (!j->row || !j->prev)
        return 1;

    uint8_t *h = IPP_MALLOC(PWG_HEADER_SIZE);
    if (!h)
        return 1;
    static const char MAGIC[4] = {'R', 'a', 'S', '2'};
    chunk_out(j, MAGIC, 4);
    pwg_header(j, h);
    chunk_out(j, h, PWG_HEADER_SIZE);
    free(h);
    return j->err ? 1 : 0;
}

// Encode one raster line into j->row; px NULL = all-white line.
static void encode_row(job_t *j, const uint8_t *px)
{
    uint8_t *o = j->row;
    const int bpp = j->bpp;
    const int w = j->w;

    if (!px) {
        *o++ = 128;                     // whole line white
        j->rlen = o - j->row;
        return;
    }

    // last non-white pixel index (white = all bytes 0xff)
    int xend = w;
    while (xend > 0) {
        const uint8_t *q = px + (size_t)(xend - 1) * bpp;
        int b;
        for (b = 0; b < bpp && q[b] == 0xff; b++)
            ;
        if (b < bpp)
            break;
        xend--;
    }

    int x = 0;                          // pixel cursor
    while (x < xend) {
        // length of the identical-pixel run starting at x (cap 128)
        int run = 1;
        const uint8_t *first = px + (size_t)x * bpp;
        while (x + run < xend && run < 128 &&
               memcmp(px + (size_t)(x + run) * bpp, first, bpp) == 0)
            run++;
        if (run >= 3 || (bpp == 3 && run >= 2)) {
            *o++ = run - 1;
            memcpy(o, first, bpp);
            o += bpp;
            x += run;
            continue;
        }
        // literal run until an identical-pixel run worth encoding
        int lstart = x, lit = 0;
        while (x < xend && lit < 128) {
            int r = 1;
            while (x + r < xend && r < 128 &&
                   memcmp(px + (size_t)(x + r) * bpp, px + (size_t)x * bpp, bpp) == 0)
                r++;
            if (r >= 3 || (bpp == 3 && r >= 2))
                break;
            x += r;
            lit += r;
        }
        if (lit == 1) {                 // a lone pixel: a 1-px run packet
            *o++ = 0;
            memcpy(o, px + (size_t)lstart * bpp, bpp);
            o += bpp;
        } else if (lit > 1) {
            *o++ = 257 - lit;
            memcpy(o, px + (size_t)lstart * bpp, (size_t)lit * bpp);
            o += (size_t)lit * bpp;
        }
    }
    if (xend < w)
        *o++ = 128;                     // the rest is white
    j->rlen = o - j->row;
}

static int page_line(void *job, int y, const uint8_t *px)
{
    job_t *j = job;
    (void)y;
    encode_row(j, px);
    if (j->plen == j->rlen && memcmp(j->prev, j->row, j->rlen) == 0 && j->rep < 255) {
        j->rep++;
        return j->err ? 4 : 0;
    }
    // different row (or rep saturated): emit the pending one
    uint8_t r = j->rep;
    if (j->plen && (chunk_out(j, &r, 1) || chunk_out(j, j->prev, j->plen)))
        return 4;
    memcpy(j->prev, j->row, j->rlen);
    j->plen = j->rlen;
    j->rep = 0;
    j->page_lines++;
    return j->err ? 4 : 0;
}

static int page_end(void *job)
{
    job_t *j = job;
    uint8_t r = j->rep;
    if (j->plen && (chunk_out(j, &r, 1) || chunk_out(j, j->prev, j->plen)))
        return 4;
    j->plen = 0;
    free(j->row);
    free(j->prev);
    j->row = j->prev = NULL;
    return j->err ? 4 : 0;
}

static int job_end(void *job)
{
    job_t *j = job;
    if (!j->err && j->plen) {           // a page left unflushed (job cancelled early)
        uint8_t r = j->rep;
        chunk_out(j, &r, 1);
        chunk_out(j, j->prev, j->plen);
    }
    if (!j->err)
        chunk_flush(j);
    if (!j->err)
        wr(j, "0\r\n\r\n", 5);          // last chunk
    if (!j->err && j->io->sync)
        j->io->sync(j->io->ctx, "HTTP/1.1");   // the printer's HTTP response
    int err = j->err;
    free(j->row);
    free(j->prev);
    free(j->chunk);
    free(j);
    return err;
}

// ------------------------------------------------------------- test page ---

// A minimal raster test page, drawn through the driver's own path so the
// BOOT button works on IPP-over-USB printers too: border at ~8 mm, a ramp of
// 16 grey steps and a solid bar.
static int test_page(const printer_driver_t *d, const printer_io_t *io, int dpi, const char *label)
{
    (void)label;
    const printer_paper_t *paper = &d->papers[0];
    void *job = job_begin(d, io, dpi, 0);
    if (!job)
        return -1;
    job_t *j = job;
    const double mm = dpi / 25.4;
    const int w = (int)(paper->width / 100.0 * mm);
    const int h = (int)(paper->height / 100.0 * mm);
    if (page_begin(job, w, h, 1))
        goto out;
    uint8_t *line = malloc((size_t)w);
    if (!line)
        goto out;
    const int m = (int)(8 * mm), t = (int)(0.5 * mm);
    const int ry = (int)(40 * mm), rh = (int)(14 * mm);
    const int rx0 = (int)(15 * mm), rx1 = w - (int)(15 * mm);
    const int by = (int)(60 * mm), bh = (int)(5 * mm);
    for (int y = 0; y < h && !j->err; y++) {
        memset(line, 0xff, w);
        if ((y >= m && y < m + t) || (y >= h - m - t && y < h - m))
            memset(line + m, 0, w - 2 * m);
        if (y >= m && y < h - m) {
            memset(line + m, 0, t);
            memset(line + w - m - t, 0, t);
        }
        if (y >= ry && y < ry + rh) {   // 16-step grey ramp
            for (int k = 0; k < 16; k++) {
                int x0 = rx0 + (rx1 - rx0) * k / 16, x1 = rx0 + (rx1 - rx0) * (k + 1) / 16;
                if (x1 > w)
                    x1 = w;
                memset(line + x0, k * 17, x1 - x0);
            }
        }
        if (y >= by && y < by + bh)
            memset(line + rx0, 0, rx1 - rx0);
        if (page_line(job, y, line))
            break;
        if (io->progress && (y & 63) == 63)
            io->progress(io->ctx, y, h);
    }
    free(line);
    if (!j->err)
        page_end(job);
out:
    return job_end(job);
}

const printer_driver_t ipp_usb_driver = {
    .id = "ipp-usb",
    .make_model = "IPP-over-USB printer",
    .usb_mfg = NULL,
    .usb_mdl = NULL,
    .usb_vid = 0,
    .usb_pid = 0,
    .color = true,
    .scanner = false,
    .resolutions = {300, 600},
    .nresolutions = 2,
    .papers = PAPERS,
    .npapers = NPAPERS,
    .margin_left = 0,
    .margin_right = 0,
    .margin_top = 0,
    .margin_bottom = 0,
    .model = NULL,
    .job_begin = job_begin,
    .page_begin = page_begin,
    .page_line = page_line,
    .page_end = page_end,
    .job_end = job_end,
    .job_stats = NULL,
    .test_page = test_page,
    .status = NULL,
};
