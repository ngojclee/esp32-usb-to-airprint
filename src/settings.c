#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "local_config.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NS = "settings";
static const char *KEY = "v1";

static settings_t s_settings;

#define COPY(dst, src) snprintf((dst), sizeof(dst), "%s", (src))

static void defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    COPY(s->ssid, WIFI_SSID);
    COPY(s->pass, WIFI_PASSWORD);
    COPY(s->hostname, HOSTNAME[0] ? HOSTNAME : "airprint");
    COPY(s->name, PRINTER_NAME);      // "": the printer's model
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");

    defaults(&s_settings);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "none saved: using the build defaults");
        return ESP_OK;
    }
    settings_t saved;
    size_t len = sizeof(saved);
    if (nvs_get_blob(h, KEY, &saved, &len) == ESP_OK && len == sizeof(saved)) {
        // never trust stored strings to be terminated
        saved.ssid[sizeof(saved.ssid) - 1] = 0;
        saved.pass[sizeof(saved.pass) - 1] = 0;
        saved.hostname[sizeof(saved.hostname) - 1] = 0;
        saved.name[sizeof(saved.name) - 1] = 0;
        saved.admin[sizeof(saved.admin) - 1] = 0;
        saved.ip[sizeof(saved.ip) - 1] = 0;
        saved.mask[sizeof(saved.mask) - 1] = 0;
        saved.gw[sizeof(saved.gw) - 1] = 0;
        saved.dns[sizeof(saved.dns) - 1] = 0;
        s_settings = saved;
        ESP_LOGI(TAG, "loaded: host %s, %s", s_settings.hostname, s_settings.static_ip ? "static IP" : "DHCP");
    } else {
        ESP_LOGW(TAG, "saved settings unreadable: using the build defaults");
    }
    nvs_close(h);
    return ESP_OK;
}

const settings_t *settings_get(void)
{
    return &s_settings;
}

esp_err_t settings_save(const settings_t *s)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_blob(h, KEY, s, sizeof(*s));
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK)
        s_settings = *s;
    return err;
}

esp_err_t settings_erase(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool settings_hostname_ok(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > 32 || s[0] == '-' || s[n - 1] == '-')
        return false;
    for (size_t i = 0; i < n; i++)
        if (!(islower((unsigned char)s[i]) || isdigit((unsigned char)s[i]) || s[i] == '-'))
            return false;
    return true;
}

bool settings_ipv4_ok(const char *s)
{
    int part = 0, digits = 0, value = 0;
    for (const char *p = s;; p++) {
        if (isdigit((unsigned char)*p)) {
            value = value * 10 + (*p - '0');
            if (++digits > 3 || value > 255)
                return false;
        } else if (*p == '.' || *p == 0) {
            if (!digits)
                return false;
            part++;
            if (*p == 0)
                return part == 4;
            if (part > 3)
                return false;
            digits = value = 0;
        } else {
            return false;
        }
    }
}
