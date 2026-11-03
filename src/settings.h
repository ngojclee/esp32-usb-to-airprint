// Runtime settings (WiFi, network, names), kept in NVS and edited from the
// web page /setup. The values in .env are only the defaults of a fresh board,
// so a build with an empty .env works for anyone: it starts the setup WiFi.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    char ssid[33];
    char pass[65];
    char hostname[33];          // DHCP / mDNS name: <hostname>.local
    char name[64];              // printer name shown by AirPrint / IPP; "" = its model
    char admin[33];             // password asked by /setup; "" = none
    bool static_ip;
    char ip[16], mask[16], gw[16], dns[16];
} settings_t;

// Initialises NVS and loads the settings (defaults from .env if none saved).
esp_err_t settings_init(void);
const settings_t *settings_get(void);
// Saves to NVS; takes effect after a restart.
esp_err_t settings_save(const settings_t *s);
// Forgets everything saved: the next boot uses the .env defaults.
esp_err_t settings_erase(void);

// A valid DNS label: 1-32 of [a-z0-9-], not starting or ending with '-'.
bool settings_hostname_ok(const char *s);
// A dotted IPv4 address.
bool settings_ipv4_ok(const char *s);
