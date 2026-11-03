// Host capture of the driver's own test page, used to find the byte the
// printer reports as corrupt (POSITION 0x7595).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "printer_driver.h"

extern const printer_driver_t dell_b1160_driver;

static FILE *g_out;

static int sink(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    return fwrite(data, 1, len, g_out) == len ? 0 : -1;
}

int main(int argc, char **argv)
{
    int dpi = argc > 1 ? atoi(argv[1]) : 1200;
    g_out = fopen("testpage.prn", "wb");
    if (!g_out)
        return 1;
    printer_io_t io = {.write = sink, .sync = NULL, .progress = NULL, .ctx = NULL};
    int rc = dell_b1160_driver.test_page(&dell_b1160_driver, &io, dpi, "");
    fclose(g_out);
    printf("test_page rc=%d\n", rc);
    return rc;
}
