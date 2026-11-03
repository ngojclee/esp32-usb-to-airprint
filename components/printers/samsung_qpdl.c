// Dell B1160 / Samsung ML-216x series: SPL3, which is Samsung's QPDL v3.
//
// 1200 dpi, 1 bit per pixel, pages cut into bands of 128 lines. Every band is
// sent transposed (one 128-byte column after another), bit-inverted (0 = a
// black dot) and compressed with Samsung's algorithm 0x11 (a small LZ77 with
// a 64-entry table of byte distances).
//
// The stream layout was worked out from captures of the printer's own
// drivers and the format notes of the SpliX project (GPL, not used as code);
// the encoder here is an independent implementation.
//
// Layout (see docs/research-dell-b1160.md):
//
//   PJL header
//   page header    17 bytes, big-endian fields
//     bands         header 11 bytes (0x0c, band, width px, height, 0x11, size)
//                  sub-header EF CD AB 09, data, checksum (big-endian)
//   page footer     3 bytes (0x01, copies)
//   PJL footer
//
// The band origin is the printable corner: the printer's hard margin is
// removed from the top and the left of the page raster, exactly as SpliX
// does for the ml2165.ppd that drives this printer under CUPS today.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "shared_buf.h"
#define QLOG(...) ESP_LOGI("qpdl", __VA_ARGS__)
#else
#define QLOG(...) do { } while (0)
#endif

#include "printer_driver.h"

#define BAND_HEIGHT 128

// ImageableArea of the PPD: 12.5 pt on every side.
#define HARD_MARGIN_PT_NUM 25   // 12.5 pt = 25/2
#define HARD_MARGIN_PT_DEN 2

// algorithm 0x11
#define QPDL_TABLE 64           // distances in a band's table
#define QPDL_LEAD 128           // leading bytes stored as is
// Longest literal run per token. The format allows 128, but the B1160 times
// out mid-page on bands with long runs (dithered grey); every stream it
// printed had runs of at most 18 bytes.
#define QPDL_RAW_RUN 16
#define QPDL_MIN_COPY 3         // shortest copy a token can say
#define QPDL_MAX_MATCH (QPDL_MIN_COPY + 0x1FF)

typedef struct {
    const char *name;
    const char *pwg;
    int width, height;          // 1/100 mm
    int w_pt, h_pt;             // PPD page size, points
    int paper_type;             // QPDL paper type byte
} qpdl_paper_t;

static const qpdl_paper_t PAPERS[] = {
    {"A4", "iso_a4_210x297mm", 21000, 29700, 595, 842, 2},
    {"Letter", "na_letter_8.5x11in", 21590, 27940, 612, 792, 0},
    {"A6", "iso_a6_105x148mm", 10500, 14800, 297, 420, 17},
    {"A5", "iso_a5_148x210mm", 14800, 21000, 420, 595, 16},
    {"B5 (JIS)", "jis_b5_182x257mm", 18200, 25700, 516, 729, 11},
    {"Legal", "na_legal_8.5x14in", 21590, 35560, 612, 1008, 1},
    {"Executive", "na_executive_7.25x10.5in", 18415, 26670, 522, 756, 3},
    {"Folio", "na_foolscap_8.5x13in", 21590, 33020, 612, 936, 24},
    {"Envelope #10", "na_number-10_4.125x9.5in", 10478, 24130, 297, 684, 6},
    {"Envelope DL", "iso_dl_110x220mm", 11000, 22000, 312, 624, 9},
    {"Envelope C5", "iso_c5_162x229mm", 16200, 22900, 459, 649, 8},
    {"Oficio", "na_oficio_8.5x13.4in", 21590, 34036, 612, 965, 28},
    {"B5 (ISO)", "iso_b5_176x250mm", 17600, 25000, 499, 709, 12},
    {"Statement", "na_invoice_5.5x8.5in", 13970, 21590, 396, 612, 30},
    {"Envelope Monarch", "na_monarch_3.875x7.5in", 9843, 19050, 279, 540, 7},
    {"Envelope C6", "iso_c6_114x162mm", 11400, 16200, 323, 459, 23},
    {"Index card 3x5", "na_index-3x5_3x5in", 7620, 12700, 216, 360, 27},
};
#define NPAPERS ((int)(sizeof(PAPERS) / sizeof(PAPERS[0])))

typedef struct {
    printer_io_t io;
    int dpi;
    const qpdl_paper_t *paper;

    int width_px;               // band width
    int line_bytes;
    int hard_x_px;              // left hard margin in pixels
    int hard_y;                 // top hard margin in lines
    int page_h;                 // printable lines
    int page_w_300, page_h_300; // page header fields

    int src_w, src_h, bpp;
    int next_row;               // next page row the driver expects
    int rows;                   // rows buffered in the current band
    int band_no;
    int page_open;
    int err;

    // The band, already in the printer's layout: transposed (one 128-byte
    // column per 8 pixels) and inverted (0 bit = black). pack_line writes
    // straight into it; it is all 0xFF (white) between bands.
    uint8_t *xpose;             // line_bytes * BAND_HEIGHT
    int band_dirty;             // any black dot in the current band
    uint8_t *comp;
    size_t comp_cap;
    size_t out_bytes;

    // Dot mask per (row phase, grey level): bit k set = a dot at column
    // phase k. Same thresholds as the per-pixel dither, looked up 8 at a time.
    uint8_t dot_mask[4][256];

    // Page output buffer. A laser cannot stop once the paper moves, so the
    // printer must get the page at engine speed: the whole page is built
    // here and sent in one go at page_end. If it fills up (a dense photo)
    // it is flushed early and the rest streams as before.
    uint8_t *obuf;
    size_t ocap, olen;
    int overflowed;             // this page did not fit in obuf
    int obuf_shared;            // obuf is the shared PSRAM buffer (sbuf)

    int64_t t_page0, t_send_us;     // page start, time spent in io.write
} qpdl_job_t;

// ---------------------------------------------------------------------------
// helpers

static int64_t now_us(void)
{
#ifdef ESP_PLATFORM
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#else
    return (int64_t)clock() * 1000000 / CLOCKS_PER_SEC;
#endif
}

static int io_write(qpdl_job_t *j, const void *p, size_t n)
{
    int64_t t = now_us();
    int rc = j->io.write(j->io.ctx, p, n);
    j->t_send_us += now_us() - t;
    if (rc < 0)
        j->err = -1;
    return rc < 0 ? -1 : 0;
}

static int flush_out(qpdl_job_t *j)
{
    if (j->err)
        return -1;
    if (j->olen) {
        if (io_write(j, j->obuf, j->olen) < 0)
            return -1;
        j->olen = 0;
    }
    return 0;
}

static int wr(qpdl_job_t *j, const void *p, size_t n)
{
    if (j->err)
        return -1;
    j->out_bytes += n;
    if (j->olen + n <= j->ocap) {
        memcpy(j->obuf + j->olen, p, n);
        j->olen += n;
        return 0;
    }
    // page larger than the buffer: send what we have and stream on
    if (j->obuf && !j->overflowed) {
        j->overflowed = 1;
        QLOG("page buffer full (%u bytes), streaming the rest", (unsigned)j->ocap);
    }
    if (flush_out(j) < 0)
        return -1;
    if (n <= j->ocap) {
        memcpy(j->obuf, p, n);
        j->olen = n;
        return 0;
    }
    return io_write(j, p, n);
}

static void put_be16(uint8_t *p, int v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void *job_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p)
        return p;
#endif
    return malloc(n);
}

// The page buffer: the shared PSRAM buffer (several MB, idle while printing
// unless dev mode captures the job), else the biggest block that leaves the
// rest of the firmware some PSRAM.
static uint8_t *alloc_page_buffer(size_t *cap, int *shared)
{
    *shared = 0;
#ifdef ESP_PLATFORM
    uint8_t *sb = sbuf_acquire(SBUF_PRINT, cap);
    if (sb && *cap >= (64u << 10)) {
        *shared = 1;
        return sb;
    }
    if (sb)
        sbuf_release(SBUF_PRINT, 0);
#endif
    size_t want = 2u << 20;
#ifdef ESP_PLATFORM
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t keep = 128u << 10;
    want = largest > keep ? largest - keep : 0;
    if (want > (2u << 20))
        want = 2u << 20;
#endif
    for (; want >= (32u << 10); want /= 2) {
        uint8_t *p = job_alloc(want);
        if (p) {
            *cap = want;
            return p;
        }
    }
    *cap = 0;
    return NULL;
}

static int ceil_div(int a, int b)
{
    return (a + b - 1) / b;
}

// 4x4 ordered dither: 0 = black, 255 = white
static const uint8_t BAYER[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static const char PJL_HEAD[] =
    "\x1b%-12345X"
    "@PJL DEFAULT SERVICEDATE=20261103\n"
    "@PJL SET USERNAME=\"esp32\"\n"
    "@PJL SET JOBNAME=\"AirPrint\"\n"
    "@PJL DEFAULT POWERSAVE=OFF\n"
    "@PJL SET JAMRECOVERY=OFF\n"
    "@PJL SET DUPLEX=OFF\n"
    "@PJL SET PAPERTYPE=OFF\n"
    "@PJL SET ALTITUDE=LOW\n"
    "@PJL SET DENSITY=3\n"
    "@PJL SET RET=NORMAL\n"
    "@PJL ENTER LANGUAGE = QPDL\n";

static const char PJL_TAIL[] = "\x09\x1b%-12345X";

// ---------------------------------------------------------------------------
// algorithm 0x11

// The distance table is fixed rather than tuned per band. A band is stored
// column by column (the 128 rows of one byte column, then the next one), so
// useful back references are short (a pattern repeating down a column) or a
// whole number of columns (the same row further left), give or take a byte.
// Most likely first: the search stops at the first longest match.
static const uint16_t QPDL_DIST[QPDL_TABLE] = {
    1,    128,  256,  384,  2,    3,    4,    512,  640,  768,  896,  1024, 5,    6,    7,    8,
    127,  129,  255,  257,  383,  385,  511,  513,  9,    10,   11,   12,   13,   14,   15,   16,
    1152, 1280, 1408, 1536, 1664, 1792, 1920, 2048, 639,  641,  767,  769,  895,  897,  1023, 1025,
    32,   64,   1151, 1153, 1279, 1281, 1407, 1409, 1535, 1537, 1663, 1665, 1791, 1793, 1919, 1921,
};

static void put_le16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

// Encodes n bytes; returns the encoded size, or -1 if cap is too small.
//
//   u32 LE   number of leading bytes copied as is
//   64 x u16 LE  distance table
//   leading bytes
//   tokens:  0lllllll + 1..128 bytes   literal run of l+1 bytes
//            1xxxxxxx yyiiiiii         copy length 3 + (yy:xxxxxxx) bytes
//                                      from distance table[i] back
static int qpdl_encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t lead = n < QPDL_LEAD ? n : QPDL_LEAD;
    size_t w = 4 + 2 * QPDL_TABLE;
    if (cap < w + lead)
        return -1;
    put_le16(out, (unsigned)lead);
    put_le16(out + 2, 0);
    for (int k = 0; k < QPDL_TABLE; k++)
        put_le16(out + 4 + 2 * k, QPDL_DIST[k]);
    memcpy(out + w, in, lead);
    w += lead;

    size_t pos = lead;
    size_t lit_at = 0;          // count byte of the open literal run
    unsigned lit = 0;           // bytes in it (0: no run open)
    while (pos < n) {
        size_t limit = n - pos < QPDL_MAX_MATCH ? n - pos : QPDL_MAX_MATCH;
        const uint8_t *cur = in + pos;
        size_t best = 0;
        int best_k = 0;
        if (limit >= QPDL_MIN_COPY) {
            for (int k = 0; k < QPDL_TABLE; k++) {
                size_t d = QPDL_DIST[k];
                if (d > pos)
                    continue;
                const uint8_t *src = cur - d;
                // to beat `best`, byte `best` must match too: test it first
                if (src[best] != cur[best])
                    continue;
                size_t len = 0;
                while (len < limit && src[len] == cur[len])
                    len++;
                if (len > best) {
                    best = len;
                    best_k = k;
                    if (len == limit)
                        break;
                }
            }
        }

        if (best >= QPDL_MIN_COPY) {
            if (w + 2 > cap)
                return -1;
            unsigned v = (unsigned)(best - QPDL_MIN_COPY);   // 9 bits
            out[w++] = (uint8_t)(0x80 | (v & 0x7F));
            out[w++] = (uint8_t)(((v >> 7) << 6) | (unsigned)best_k);
            pos += best;
            lit = 0;
            continue;
        }

        if (lit == 0) {
            if (w + 2 > cap)
                return -1;
            lit_at = w++;
        } else if (w + 1 > cap) {
            return -1;
        }
        out[w++] = in[pos++];
        out[lit_at] = (uint8_t)lit;         // run length - 1
        if (++lit == QPDL_RAW_RUN)
            lit = 0;
    }
    return (int)w;
}

// ---------------------------------------------------------------------------
// bands

// Ordered dither for midtones, but near-black and near-white stay solid so
// thin rules and text stay crisp. Bit c of dot_mask[row & 3][v] = a dot at
// a column with (x & 3) == c.
static void build_dot_mask(qpdl_job_t *j)
{
    for (int r = 0; r < 4; r++)
        for (int v = 0; v < 256; v++) {
            uint8_t m = 0;
            for (int c = 0; c < 4; c++) {
                int dot;
                if (v < 48)
                    dot = 1;
                else if (v > 208)
                    dot = 0;
                else
                    dot = v < BAYER[r][c] * 16 + 8;
                if (dot)
                    m |= (uint8_t)(1 << c);
            }
            j->dot_mask[r][v] = m;
        }
}

// One source line into row band_row of the transposed, inverted band.
// White bytes are skipped: the band buffer starts out all white.
static void pack_line(qpdl_job_t *j, int band_row, const uint8_t *px, int page_row)
{
    if (!px)
        return;

    // hard_x_px is a multiple of 8, so a column's dither phase is (x & 3)
    const uint8_t *mask = j->dot_mask[page_row & 3];
    uint8_t *col = j->xpose + band_row;
    int step = j->bpp == 3 ? 3 : 1;
    const uint8_t *src = px + (size_t)j->hard_x_px * step;
    int xend = j->src_w - j->hard_x_px;
    if (xend > j->width_px)
        xend = j->width_px;
    if (xend <= 0)
        return;

    int dirty = 0;
    int bx = 0;
    if (step == 1) {
        // grey: 8 pixels per output byte, all-white runs cost one AND chain
        int full = xend >> 3;
        for (const uint8_t *p = src; bx < full; bx++, p += 8) {
            if ((p[0] & p[1] & p[2] & p[3] & p[4] & p[5] & p[6] & p[7]) == 0xFF)
                continue;
            uint8_t b = (uint8_t)(((mask[p[0]] << 7) & 0x80) | ((mask[p[1]] << 5) & 0x40) |
                                  ((mask[p[2]] << 3) & 0x20) | ((mask[p[3]] << 1) & 0x10) |
                                  ((mask[p[4]] << 3) & 0x08) | ((mask[p[5]] << 1) & 0x04) |
                                  ((mask[p[6]] >> 1) & 0x02) | ((mask[p[7]] >> 3) & 0x01));
            if (b) {
                col[(size_t)bx * BAND_HEIGHT] = (uint8_t)~b;   // the printer prints a 0 bit
                dirty = 1;
            }
        }
    }
    // RGB, and the last partial byte of a grey line
    int nbytes = (xend + 7) >> 3;
    for (; bx < nbytes; bx++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) {
            int x = bx * 8 + k;
            if (x >= xend)
                break;
            const uint8_t *q = src + (size_t)x * step;
            int v = step == 3 ? (q[0] * 77 + q[1] * 150 + q[2] * 29) >> 8 : q[0];
            if ((mask[v] >> (k & 3)) & 1)
                b |= (uint8_t)(0x80 >> k);
        }
        if (b) {
            col[(size_t)bx * BAND_HEIGHT] = (uint8_t)~b;
            dirty = 1;
        }
    }
    j->band_dirty |= dirty;
}

static int emit_band(qpdl_job_t *j)
{
    if (j->rows == 0)
        return 0;

    size_t n = (size_t)j->line_bytes * BAND_HEIGHT;
    if (j->band_dirty) {
        int clen = qpdl_encode(j->xpose, n, j->comp, j->comp_cap);
        if (clen < 0) {
            j->err = -1;
            return -1;
        }

        uint8_t h[11];
        h[0] = 0x0C;
        h[1] = (uint8_t)j->band_no;
        put_be16(h + 2, j->width_px);
        put_be16(h + 4, BAND_HEIGHT);
        h[6] = 0x11;
        put_be32(h + 7, (uint32_t)clen + 8);

        static const uint8_t sig[4] = {0xEF, 0xCD, 0xAB, 0x09};
        uint32_t sum = 0xEF + 0xCD + 0xAB + 0x09;
        for (int i = 0; i < clen; i++)
            sum += j->comp[i];
        uint8_t chk[4];
        put_be32(chk, sum);

        if (wr(j, h, sizeof(h)) < 0 || wr(j, sig, sizeof(sig)) < 0 ||
            wr(j, j->comp, (size_t)clen) < 0 || wr(j, chk, sizeof(chk)) < 0)
            return -1;

        memset(j->xpose, 0xFF, n);          // white again for the next band
        j->band_dirty = 0;
    }

    j->band_no++;
    j->rows = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// printer_driver_t

static void job_free(qpdl_job_t *j)
{
    free(j->xpose);
    free(j->comp);
#ifdef ESP_PLATFORM
    if (j->obuf_shared)
        sbuf_release(SBUF_PRINT, 0);
    else
#endif
        free(j->obuf);
    free(j);
}

static void *job_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper)
{
    (void)d;
    if (paper < 0 || paper >= NPAPERS)
        paper = 0;

    qpdl_job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->io = *io;
    j->dpi = dpi;
    j->paper = &PAPERS[paper];

    // QPDL v3: the page header carries the raster size in pixels, not the
    // 1/300 inch size that v1/v2 used (SpliX: width = page->width()).
    j->width_px = ceil_div(j->paper->w_pt * dpi, 72);
    j->width_px = (j->width_px + 7) & ~7;
    j->line_bytes = j->width_px / 8;

    int hx = ceil_div(HARD_MARGIN_PT_NUM * dpi, HARD_MARGIN_PT_DEN * 72);
    hx = (hx + 7) & ~7;
    j->hard_x_px = hx;
    j->hard_y = ceil_div(HARD_MARGIN_PT_NUM * dpi, HARD_MARGIN_PT_DEN * 72);

    int ph = ceil_div(j->paper->h_pt * dpi, 72);
    j->page_h = ph - j->hard_y;
    if (j->page_h < 1)
        j->page_h = 1;

    j->page_w_300 = ceil_div(j->paper->w_pt * 300, 72);
    j->page_h_300 = ceil_div(j->paper->h_pt * 300, 72);

    size_t band = (size_t)j->line_bytes * BAND_HEIGHT;
    j->comp_cap = band + band / QPDL_RAW_RUN + 1024;   // worst case: all literals
    j->xpose = job_alloc(band);
    j->comp = job_alloc(j->comp_cap);
    if (!j->xpose || !j->comp) {
        job_free(j);
        return NULL;
    }
    memset(j->xpose, 0xFF, band);
    build_dot_mask(j);
    j->obuf = alloc_page_buffer(&j->ocap, &j->obuf_shared);   // NULL: stream as we go

    if (wr(j, PJL_HEAD, sizeof(PJL_HEAD) - 1) < 0) {
        job_free(j);
        return NULL;
    }
    QLOG("job: %s %d dpi, %d px/line, %d lines, margin %d/%d, page buffer %u KB",
         j->paper->name, dpi, j->width_px, j->page_h, j->hard_x_px, j->hard_y,
         (unsigned)(j->ocap >> 10));
    return j;
}

static int page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    qpdl_job_t *j = job;
    j->src_w = src_w;
    j->src_h = src_h;
    j->bpp = bytes_pp;
    j->next_row = 0;
    j->rows = 0;
    j->band_no = 0;
    j->page_open = 1;
    j->overflowed = 0;
    j->t_page0 = now_us();
    j->t_send_us = 0;

    uint8_t h[17];
    memset(h, 0, sizeof(h));
    h[0] = 0;                               // page header
    h[1] = (uint8_t)(j->dpi / 100);         // y resolution
    put_be16(h + 2, 1);                     // copies
    h[4] = (uint8_t)j->paper->paper_type;
    put_be16(h + 5, j->width_px);           // printable area, pixels (QPDL v3)
    put_be16(h + 7, j->page_h);             // (v1/v2 used 1/300 inch here)
    h[9] = 1;                               // paper source: auto
    h[10] = 0;
    h[11] = 1;                              // simplex (SpliX QPDL v3 sends 1)
    h[12] = 0;                              // tumble
    h[13] = 0;
    h[14] = 3;                              // QPDL version
    h[15] = 1;                              // colour planes
    h[16] = (uint8_t)(j->dpi / 100);        // x resolution

    QLOG("page: src %dx%d bpp %d, header %dx%d, %d bands",
         src_w, src_h, bytes_pp, j->width_px, j->page_h,
         (j->page_h + BAND_HEIGHT - 1) / BAND_HEIGHT);

    return wr(j, h, sizeof(h));
}

static int page_line(void *job, int y, const uint8_t *px)
{
    qpdl_job_t *j = job;
    if (j->err)
        return -1;

    int by = y - j->hard_y;
    if (by < 0 || by >= j->page_h)
        return 0;

    while (j->next_row < by) {
        pack_line(j, j->rows, NULL, j->next_row + j->hard_y);
        j->rows++;
        j->next_row++;
        if (j->rows == BAND_HEIGHT && emit_band(j) < 0)
            return -1;
    }
    if (by == j->next_row) {
        pack_line(j, j->rows, px, y);
        j->rows++;
        j->next_row++;
        if (j->rows == BAND_HEIGHT && emit_band(j) < 0)
            return -1;
    }
    return j->err ? -1 : 0;
}

static int page_end(void *job)
{
    qpdl_job_t *j = job;
    if (j->err)
        return -1;
    if (emit_band(j) < 0)
        return -1;

    uint8_t h[3] = {1, 0, 1};               // page footer, copies (SpliX layout)
    j->page_open = 0;
    if (wr(j, h, sizeof(h)) < 0)
        return -1;

    // the whole page goes out now, at USB speed
    int64_t render = now_us() - j->t_page0 - j->t_send_us;
    size_t buffered = j->olen;
    if (flush_out(j) < 0)
        return -1;
    QLOG("page done: %d bands, render %d ms, send %d ms, %u bytes sent at end%s",
         j->band_no, (int)(render / 1000), (int)(j->t_send_us / 1000),
         (unsigned)buffered, j->overflowed ? " (buffer overflowed)" : "");
    (void)render;
    (void)buffered;
    return 0;
}

static int job_end(void *job)
{
    qpdl_job_t *j = job;
    if (j->page_open && !j->err)
        page_end(j);
    if (!j->err)
        wr(j, PJL_TAIL, sizeof(PJL_TAIL) - 1);
    flush_out(j);

    int ret = j->err ? -1 : 0;
    QLOG("job end: %d bands, %u bytes, err %d", j->band_no, (unsigned)j->out_bytes, j->err);
    job_free(j);
    return ret;
}

static void job_stats(void *job, char *buf, size_t n)
{
    qpdl_job_t *j = job;
    snprintf(buf, n, "band %d, %d bytes", j->band_no, j->width_px / 8);
}

// A4 test page: border, margin boxes at 10 mm (solid) and 20 mm (dashed)
// from the paper edges, 16 step grey ramp, solid bar.
// Printed by the board's button (double press), so it needs no WiFi.
static int test_page(const printer_driver_t *d, const printer_io_t *io, int dpi, const char *label)
{
    (void)label;
    qpdl_job_t *j = job_begin(d, io, dpi, 0);   // 0 = A4 (matches the loaded paper)
    if (!j)
        return -1;

    int W = j->width_px;
    int H = j->page_h;
    if (page_begin(j, W, H, 1) < 0) {
        job_end(j);
        return -1;
    }

    uint8_t *line = malloc((size_t)W);
    if (!line) {
        job_end(j);
        return -1;
    }

    // paper size in pixels, and the margin marks (see below)
    int pw = (int)((long)j->paper->width * dpi / 2540);
    int ph = (int)((long)j->paper->height * dpi / 2540);
    int mm10 = dpi * 100 / 254;
    int mm20 = 2 * mm10;
    int t = dpi / 50;                   // 0.5 mm lines
    int dash = mm10 / 5;                // 2 mm dashes

    for (int r = 0; r < H; r++) {
        // yield now and then (task watchdog), as printer_io_t asks
        if ((r & 63) == 0 && io->progress)
            io->progress(io->ctx, r, H);
        memset(line, 255, (size_t)W);
        int dirty = 0;

        // frame only: bisecting a printer "Corrupted Data" on complex bands
        if (r < 6 || r >= H - 6) {
            memset(line + j->hard_x_px, 0, (size_t)(W - j->hard_x_px));
            dirty = 1;
        }
        // grey ramp across the middle
        if (r > H / 2 - 300 && r < H / 2 + 300) {
            for (int x = j->hard_x_px + 100; x < W - 100; x++) {
                int step = (x - j->hard_x_px - 100) * 16 / (W - 200 - j->hard_x_px);
                if (step > 15)
                    step = 15;
                line[x] = (uint8_t)(step * 17);
            }
            dirty = 1;
        }

        // Margin marks in paper coordinates (raster x = paper x, raster row r
        // = paper y - hard_y): a solid box whose outer edge is 10 mm from
        // every paper edge, a dashed one at 20 mm. Measuring the 10 mm box on
        // paper gives the printer's real offsets.
        {
            int py = r + j->hard_y;
            int in10 = py >= mm10 && py < ph - mm10;
            int in20 = py >= mm20 && py < ph - mm20;
            if ((py >= mm10 && py < mm10 + t) || (py >= ph - mm10 - t && py < ph - mm10)) {
                for (int x = mm10; x < pw - mm10 && x < W; x++)
                    line[x] = 0;
                dirty = 1;
            } else if (in10) {
                for (int x = mm10; x < mm10 + t; x++)
                    line[x] = 0;
                for (int x = pw - mm10 - t; x < pw - mm10 && x < W; x++)
                    line[x] = 0;
                dirty = 1;
            }
            if ((py >= mm20 && py < mm20 + t) || (py >= ph - mm20 - t && py < ph - mm20)) {
                for (int x = mm20; x < pw - mm20 && x < W; x++)
                    if ((x / dash) % 2 == 0)
                        line[x] = 0;
                dirty = 1;
            } else if (in20 && (py / dash) % 2 == 0) {
                for (int x = mm20; x < mm20 + t; x++)
                    line[x] = 0;
                for (int x = pw - mm20 - t; x < pw - mm20 && x < W; x++)
                    line[x] = 0;
                dirty = 1;
            }
        }

        // solid bar
        if (r > H / 4 && r < H / 4 + 40) {
            memset(line + j->hard_x_px + 100, 0, (size_t)(W - 200 - j->hard_x_px));
            dirty = 1;
        }

        if (page_line(j, r + j->hard_y, dirty ? line : NULL) < 0) {
            free(line);
            job_end(j);
            return -1;
        }
    }

    free(line);
    if (page_end(j) < 0) {
        job_end(j);
        return -1;
    }
    return job_end(j);
}

static const printer_paper_t DRIVER_PAPERS[] = {
    {"A4", "iso_a4_210x297mm", 21000, 29700},
    {"Letter", "na_letter_8.5x11in", 21590, 27940},
    {"A6", "iso_a6_105x148mm", 10500, 14800},
    {"A5", "iso_a5_148x210mm", 14800, 21000},
    {"B5 (JIS)", "jis_b5_182x257mm", 18200, 25700},
    {"Legal", "na_legal_8.5x14in", 21590, 35560},
    {"Executive", "na_executive_7.25x10.5in", 18415, 26670},
    {"Folio", "na_foolscap_8.5x13in", 21590, 33020},
    {"Envelope #10", "na_number-10_4.125x9.5in", 10478, 24130},
    {"Envelope DL", "iso_dl_110x220mm", 11000, 22000},
    {"Envelope C5", "iso_c5_162x229mm", 16200, 22900},
    {"Oficio", "na_oficio_8.5x13.4in", 21590, 34036},
    {"B5 (ISO)", "iso_b5_176x250mm", 17600, 25000},
    {"Statement", "na_invoice_5.5x8.5in", 13970, 21590},
    {"Envelope Monarch", "na_monarch_3.875x7.5in", 9843, 19050},
    {"Envelope C6", "iso_c6_114x162mm", 11400, 16200},
    {"Index card 3x5", "na_index-3x5_3x5in", 7620, 12700},
};
// one entry per paper, same order
_Static_assert(sizeof(DRIVER_PAPERS) / sizeof(DRIVER_PAPERS[0]) == sizeof(PAPERS) / sizeof(PAPERS[0]),
               "DRIVER_PAPERS and PAPERS differ");

const printer_driver_t dell_b1160_driver = {
    .id = "dell-b1160",
    .make_model = "Dell B1160 Mono Laser Printer",
    .usb_mfg = "Dell",
    .usb_mdl = "B1160 Mono Laser Printer",
    .usb_vid = 0x413c,
    .usb_pid = 0x5641,
    .color = false,
    .scanner = false,
    .resolutions = {1200},          // the PPD's default; SpliX streams at 1200 print
    .nresolutions = 1,
    .papers = DRIVER_PAPERS,
    .npapers = NPAPERS,
    .margin_left = 441,
    .margin_right = 441,
    .margin_top = 441,
    .margin_bottom = 441,
    .model = NULL,
    .job_begin = job_begin,
    .page_begin = page_begin,
    .page_line = page_line,
    .page_end = page_end,
    .job_end = job_end,
    .job_stats = job_stats,
    .test_page = test_page,
    .status = NULL,
};
