# Printer drivers (printer_driver.h): one source file per printer family.
idf_component_register(SRCS "printers.c" "canon_pixma.c" "samsung_qpdl.c" "ipp_usb.c"
                       REQUIRES airprint canon_bjl scanner)
