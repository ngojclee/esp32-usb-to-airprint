// Helpers shared by every printer driver (printer_driver.h).
#include "printer_driver.h"

#include <stdio.h>
#include <stdlib.h>

int printer_driver_paper_for(const printer_driver_t *d, int width_px, int height_px, int dpi)
{
    // 1/100 mm; the paper closest in both width and height, e.g. a Letter
    // page (215.9 x 279.4 mm) is closer to Letter than to A4 (210 x 297 mm),
    // and A5 (148 x 210) is not mistaken for Statement (139.7 x 215.9)
    long w = (long)width_px * 2540 / dpi, h = (long)height_px * 2540 / dpi;
    int best = 0;
    long best_d = -1;
    for (int i = 0; i < d->npapers; i++) {
        long dist = labs(d->papers[i].width - w) + labs(d->papers[i].height - h);
        if (best_d < 0 || dist < best_d) {
            best = i;
            best_d = dist;
        }
    }
    return best;
}

void printer_driver_urf(const printer_driver_t *d, char *buf, size_t n)
{
    // V1.4, one copy, 8-bit gray (+ sRGB), the resolutions, plain paper, 3 qualities
    int lo = d->resolutions[0], hi = d->resolutions[d->nresolutions - 1];
    char rs[24];
    if (lo == hi)
        snprintf(rs, sizeof(rs), "%d", lo);
    else
        snprintf(rs, sizeof(rs), "%d-%d", lo, hi);
    snprintf(buf, n, "V1.4,CP1,W8%s,RS%s,MT1,PQ3-4-5", d->color ? ",SRGB24" : "", rs);
}

bool printer_driver_has_dpi(const printer_driver_t *d, int dpi)
{
    for (int i = 0; i < d->nresolutions; i++)
        if (d->resolutions[i] == dpi)
            return true;
    return false;
}
