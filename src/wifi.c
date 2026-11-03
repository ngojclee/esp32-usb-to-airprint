#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "wifi_sta";

// The setup access point comes up when the configured network has not been
// joined for this long (and goes away once it is).
#define FALLBACK_AP_AFTER_US (120LL * 1000 * 1000)

static volatile bool s_connected;
static volatile bool s_ap_active;
static bool s_have_sta;
static int s_retries;
static esp_netif_t *s_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_fallback_timer;
static char s_ap_name[33];
static TaskHandle_t s_dns_task;

// ---------------------------------------------------------------------------
// Captive portal DNS: every name resolves to the setup page's address, so a
// phone joining the setup WiFi opens /setup on its own.

static void dns_task(void *arg)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53)};
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "captive DNS: cannot bind port 53");
        if (fd >= 0)
            close(fd);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    while (1) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || !s_ap_active)
            continue;
        if (buf[2] & 0x80 || buf[4] != 0 || buf[5] != 1)   // a response, or not one question
            continue;
        // end of the question: QNAME labels, then QTYPE + QCLASS
        int p = 12;
        while (p < n && buf[p])
            p += buf[p] + 1;
        p += 5;
        if (p > n)
            continue;
        uint16_t qtype = (uint16_t)(buf[p - 4] << 8 | buf[p - 3]);
        buf[2] = 0x84;                          // response, authoritative
        buf[3] = 0x00;
        buf[6] = 0;                             // answers: 1 for A, 0 otherwise
        buf[7] = qtype == 1 ? 1 : 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        if (qtype == 1) {
            static const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
            memcpy(buf + p, ans, sizeof(ans));
            p += sizeof(ans);
        }
        sendto(fd, buf, p, 0, (struct sockaddr *)&from, fl);
    }
}

// ---------------------------------------------------------------------------
// setup access point

static void ap_up(void)
{
    if (s_ap_active)
        return;
    wifi_config_t ap = {0};
    size_t len = strnlen(s_ap_name, sizeof(ap.ap.ssid));
    memcpy(ap.ap.ssid, s_ap_name, len);
    ap.ap.ssid_len = (uint8_t)len;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;
    esp_wifi_set_mode(WIFI_MODE_APSTA);         // the station keeps trying (and can scan)
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_ap_active = true;
    if (!s_dns_task)
        xTaskCreate(dns_task, "captive_dns", 3072, NULL, 3, &s_dns_task);
    ESP_LOGW(TAG, "setup WiFi \"%s\" is up: join it and open http://%s/setup", s_ap_name, WIFI_SETUP_IP);
}

static void ap_down(void)
{
    if (!s_ap_active)
        return;
    s_ap_active = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "setup WiFi off (connected)");
}

static void fallback_cb(void *arg)
{
    if (!s_connected)
        ap_up();
}

// ---------------------------------------------------------------------------

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_sta)
            esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        if (s_connected && s_have_sta)
            esp_timer_start_once(s_fallback_timer, FALLBACK_AP_AFTER_US);
        s_connected = false;
        if (s_retries++ % 10 == 0)
            ESP_LOGW(TAG, "disconnected (reason %d), retrying", d->reason);
        if (s_have_sta)
            esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_connected = true;
        esp_timer_stop(s_fallback_timer);
        ap_down();
    }
}

static esp_err_t set_static_ip(const settings_t *s)
{
    esp_netif_ip_info_t ip = {0};
    if (esp_netif_str_to_ip4(s->ip, &ip.ip) != ESP_OK || esp_netif_str_to_ip4(s->mask, &ip.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(s->gw, &ip.gw) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    esp_netif_dhcpc_stop(s_netif);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_netif, &ip), TAG, "ip");
    esp_netif_dns_info_t dns = {0};
    const char *d = s->dns[0] ? s->dns : s->gw;
    if (esp_netif_str_to_ip4(d, &dns.ip.u_addr.ip4) == ESP_OK) {
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    ESP_LOGI(TAG, "static IP %s / %s, gateway %s", s->ip, s->mask, s->gw);
    return ESP_OK;
}

esp_err_t wifi_start(const settings_t *s)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_netif, s->hostname);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_name, sizeof(s_ap_name), "Printer-Setup-%02X%02X", mac[4], mac[5]);

    // the driver's own info logs include the SSID; keep warnings only
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    esp_wifi_set_storage(WIFI_STORAGE_RAM);     // the settings live in our own NVS blob
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    const esp_timer_create_args_t targs = {.callback = fallback_cb, .name = "wifi_fallback"};
    esp_timer_create(&targs, &s_fallback_timer);

    s_have_sta = s->ssid[0] != 0;
    if (s_have_sta) {
        if (s->static_ip && set_static_ip(s) != ESP_OK)
            ESP_LOGE(TAG, "static IP settings invalid: using DHCP");
        wifi_config_t cfg = {0};
        strncpy((char *)cfg.sta.ssid, s->ssid, sizeof(cfg.sta.ssid));
        strncpy((char *)cfg.sta.password, s->pass, sizeof(cfg.sta.password));
        cfg.sta.threshold.authmode = s->pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        cfg.sta.pmf_cfg.capable = true;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "config");
        // no modem sleep: a sleeping station answers mDNS / IPP slowly
        esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
        esp_timer_start_once(s_fallback_timer, FALLBACK_AP_AFTER_US);
        ESP_LOGI(TAG, "connecting as %s", s->hostname);
    } else {
        ESP_LOGW(TAG, "no WiFi configured");
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
        ap_up();
    }

    // real wall clock for IPP date-time attributes
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    return ESP_OK;
}

bool wifi_connected(void)
{
    return s_connected;
}

bool wifi_get_ip(char *buf, size_t n)
{
    esp_netif_ip_info_t ip;
    if (!s_connected || !s_netif || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) {
        if (n)
            buf[0] = 0;
        return false;
    }
    snprintf(buf, n, IPSTR, IP2STR(&ip.ip));
    return true;
}

int wifi_rssi(void)
{
    int rssi = 0;
    if (!s_connected || esp_wifi_sta_get_rssi(&rssi) != ESP_OK)
        return 0;
    return rssi;
}

bool wifi_setup_ap_active(void)
{
    return s_ap_active;
}

const char *wifi_setup_ap_name(void)
{
    return s_ap_name;
}

int wifi_scan(wifi_net_t *nets, int max)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK)
        return 0;
    uint16_t n = 20;
    wifi_ap_record_t rec[20];
    if (esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK)
        return 0;
    // records come sorted by signal; drop duplicates and hidden networks
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (!rec[i].ssid[0])
            continue;
        bool dup = false;
        for (int j = 0; j < k && !dup; j++)
            dup = !strcmp(nets[j].ssid, (const char *)rec[i].ssid);
        if (dup)
            continue;
        memcpy(nets[k].ssid, rec[i].ssid, sizeof(nets[k].ssid));
        nets[k].ssid[sizeof(nets[k].ssid) - 1] = 0;
        nets[k].rssi = rec[i].rssi;
        nets[k].secure = rec[i].authmode != WIFI_AUTH_OPEN;
        k++;
    }
    return k;
}
