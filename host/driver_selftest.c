// Host check for the generic drivers (pcl, escpos, tspl): writes a driver's
// test page to a file, and checks the IEEE 1284 command-set matching.
//
//   driver_selftest <pcl|escpos|tspl> <out.prn>   the test page stream
//   driver_selftest cmd                            CMD matching cases
//
// tools/pcl_check.py and tools/thermal_check.py decode the streams.
// printers.c is linked with stand-ins for the drivers that need other
// components (Canon, SPL, IPP-over-USB), so only the matching is real there.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "printer_driver.h"

#define STUB(n) const printer_driver_t n = {.id = #n};
STUB(canon_mg2500_driver) STUB(canon_mg2400_driver) STUB(canon_p200_driver) STUB(canon_mg2900_driver)
STUB(canon_mg3000_driver) STUB(canon_e470_driver) STUB(canon_e460_driver) STUB(canon_ip2800_driver)
STUB(canon_e3100_driver) STUB(canon_ts3100_driver) STUB(canon_mp230_driver) STUB(canon_ip2700_driver)
STUB(canon_mp250_driver) STUB(canon_mp270_driver) STUB(canon_mp280_driver) STUB(canon_mp490_driver)
STUB(canon_mp493_driver) STUB(canon_mp495_driver) STUB(canon_mx340_driver) STUB(canon_mx350_driver)
STUB(canon_mx360_driver) STUB(canon_mx410_driver) STUB(canon_mx420_driver) STUB(dell_b1160_driver)
STUB(ipp_usb_driver)

static FILE *out;
static int wr(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    return fwrite(data, 1, len, out) == len ? 0 : -1;
}

static int cmd_tests(void)
{
    static const struct {
        const char *id, *want;
    } T[] = {
        {"MFG:HP;CMD:PJL,PML,PCL,POSTSCRIPT,PCLXL,PCL5E;MDL:HP LaserJet P2055d;", "pcl"},
        {"MFG:Brother;CMD:PJL,PCL,PCLXL;MDL:HL-L2350DW;", "pcl"},
        {"MFG:Kyocera;COMMAND SET:PJL,PCL 6,PCL 5e;MODEL:FS-1040;", "pcl"},
        {"MFG:HP;CMD:PCL3GUI,PJL,Automatic,PCL3;MDL:Deskjet 2700;", NULL},
        {"MFG:Xprinter;CMD:TSPL,ZPL,EPL;MDL:XP-D465B;", "tspl"},
        {"MFG:EPSON;CMD:ESC/POS;MDL:TM-T20II;", "escpos"},
        {"MFG:Generic;CMD:ESCPOS;MDL:POS-58;", "escpos"},
        {"MFG:Dell;CMD:SPL3,PJL;MDL:Dell B1160;", NULL},
        {"MFG:Canon;MDL:MG2500 series;", NULL},
        {"", NULL},
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        const printer_driver_t *d = printer_driver_for_cmd(T[i].id);
        const char *got = d ? d->id : NULL;
        int ok = (!got && !T[i].want) || (got && T[i].want && !strcmp(got, T[i].want));
        printf("%s  %-60.60s -> %s\n", ok ? "ok  " : "FAIL", T[i].id, got ? got : "(none)");
        fails += !ok;
    }
    return fails;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "cmd"))
        return cmd_tests() ? 1 : 0;
    if (argc != 3) {
        fprintf(stderr, "usage: driver_selftest <pcl|escpos|tspl> <out.prn> | cmd\n");
        return 2;
    }
    const printer_driver_t *d = printer_driver_find(argv[1]);
    if (!d || !d->test_page) {
        fprintf(stderr, "no driver %s\n", argv[1]);
        return 2;
    }
    out = fopen(argv[2], "wb");
    if (!out)
        return 2;
    printer_io_t io = {.write = wr};
    int dpi = d->resolutions[d->nresolutions - 1];
    int e = d->test_page(d, &io, dpi, "selftest");
    fclose(out);
    printf("%s: %s test page, %d dpi, %s -> %s\n", d->id, d->papers[0].name, dpi, e ? "FAILED" : "ok", argv[2]);
    return e ? 1 : 0;
}
