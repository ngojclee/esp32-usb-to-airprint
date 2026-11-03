// WiFi: a station with the saved settings (settings.h), plus the setup
// access point "Printer-Setup-XXXX" (open, captive portal to /setup) when no
// network is configured or the configured one stays out of reach.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "settings.h"

#define WIFI_SETUP_IP "192.168.4.1"

// Starts connecting in the background (reconnects forever).
esp_err_t wifi_start(const settings_t *s);
bool wifi_connected(void);
// The station's IPv4 address as text; false (and "") while not connected.
bool wifi_get_ip(char *buf, size_t n);
// Signal of the access point in dBm, 0 when unknown.
int wifi_rssi(void);
// The setup access point is up (and its name, for messages).
bool wifi_setup_ap_active(void);
const char *wifi_setup_ap_name(void);
typedef struct {
    char ssid[33];
    int rssi;                   // dBm
    bool secure;
} wifi_net_t;

// Scans for networks: up to max, strongest first, one entry per name.
int wifi_scan(wifi_net_t *nets, int max);
