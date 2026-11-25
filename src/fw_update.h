// Firmware updates from the settings page: an uploaded app image, or the
// latest GitHub release of CONFIG_FW_UPDATE_REPO. Both write the other OTA
// slot; the bootloader's rollback keeps the old firmware until the new one
// has run healthy for a while (debug_ota_confirm in main.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// NULL if the start of an image is an ESP32-S3 app of this project, else why not.
const char *fw_update_check_image(const uint8_t *p, size_t n);

// Writes an app image read from `read` (returns bytes, 0 at the end, < 0 on
// error) into the next OTA slot and selects it for the next boot.
// NULL = done (the caller restarts), else an error message.
const char *fw_update_from_stream(int (*read)(void *ctx, void *buf, size_t max), void *ctx);

// Background jobs (one at a time): ask GitHub for the latest release, or
// download and install the one found. false if another update is running or
// a print job holds the printer.
bool fw_update_check_start(void);
bool fw_update_install_start(void);

// {"state":"idle|checking|available|current|downloading|done|error",
//  "current":"0.2.0","latest":"0.3.0","progress":42,"error":"..."}
void fw_update_status_json(char *buf, size_t n);
