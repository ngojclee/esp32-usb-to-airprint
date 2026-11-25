#include "fw_update.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "usb_printer.h"

static const char *TAG = "fw_update";

#define API_URL "https://api.github.com/repos/" CONFIG_FW_UPDATE_REPO "/releases/latest"
#define ASSET_SUFFIX "-esp32s3-ota.bin"     // the app image attached by the release workflow
#define USER_AGENT "esp32-usb-to-airprint"
#define MAX_RELEASE_JSON (64 * 1024)
#define CHUNK 4096

enum { ST_IDLE, ST_CHECKING, ST_AVAILABLE, ST_CURRENT, ST_DOWNLOADING, ST_DONE, ST_ERROR };
static const char *const STATE_NAMES[] = {"idle", "checking", "available", "current", "downloading", "done", "error"};

static atomic_bool s_running;           // an upload or a background job holds the OTA slot
static volatile int s_state = ST_IDLE;
static volatile int s_progress;         // percent while downloading
static char s_latest[32];               // version of the latest release, no "v"
static char s_url[512];                 // its app image
static char s_err[128];

// ---------------------------------------------------------------------------

const char *fw_update_check_image(const uint8_t *p, size_t n)
{
    const size_t desc_ofs = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (n < desc_ofs + sizeof(esp_app_desc_t))
        return "image too small";
    const esp_image_header_t *ih = (const esp_image_header_t *)p;
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC)
        return "not an ESP firmware image (use the -ota.bin file of a release, or firmware.bin)";
    if (ih->chip_id != ESP_CHIP_ID_ESP32S3)
        return "image is not for the ESP32-S3";
    const esp_app_desc_t *d = (const esp_app_desc_t *)(p + desc_ofs);
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD)
        return "image has no app description";
    if (strncmp(d->project_name, esp_app_get_description()->project_name, sizeof(d->project_name)) != 0)
        return "image is from another project";
    return NULL;
}

static bool claim(void)
{
    if (usb_printer_job_active())
        return false;
    bool expected = false;
    return atomic_compare_exchange_strong(&s_running, &expected, true);
}

static void release(void)
{
    atomic_store(&s_running, false);
}

const char *fw_update_from_stream(int (*read)(void *ctx, void *buf, size_t max), void *ctx)
{
    if (!claim())
        return "busy: printing, or another update is running";
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    uint8_t *buf = malloc(CHUNK);
    const char *err = !part ? "no OTA partition" : !buf ? "out of memory" : NULL;
    esp_ota_handle_t h = 0;
    size_t done = 0;
    while (!err) {
        // fill a whole chunk: the image check needs the first bytes together
        size_t n = 0;
        int k = 0;
        while (n < CHUNK && (k = read(ctx, buf + n, CHUNK - n)) > 0)
            n += (size_t)k;
        if (k < 0) {
            err = "upload interrupted";
            break;
        }
        if (n == 0)
            break;
        if (done == 0) {
            err = fw_update_check_image(buf, n);
            if (!err && esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
                err = "cannot start the update";
            if (err)
                break;
        }
        if (done + n > part->size) {
            err = "image larger than the firmware slot";
            break;
        }
        if (esp_ota_write(h, buf, n) != ESP_OK) {
            err = "flash write failed";
            break;
        }
        done += n;
    }
    free(buf);
    if (!err && done == 0)
        err = "empty upload";
    if (!err) {
        esp_err_t e = esp_ota_end(h);
        h = 0;
        err = e == ESP_ERR_OTA_VALIDATE_FAILED ? "image is corrupted (validation failed)"
              : e != ESP_OK                    ? "update failed"
              : esp_ota_set_boot_partition(part) != ESP_OK ? "cannot select the new image"
                                                           : NULL;
    }
    if (h)
        esp_ota_abort(h);
    if (err)
        ESP_LOGW(TAG, "upload failed: %s", err);
    else
        ESP_LOGW(TAG, "uploaded firmware (%u bytes) written to %s", (unsigned)done, part->label);
    release();
    return err;
}

// ---------------------------------------------------------------------------
// GitHub releases

static void fail(const char *fmt, const char *detail)
{
    snprintf(s_err, sizeof(s_err), fmt, detail ? detail : "");
    ESP_LOGW(TAG, "%s", s_err);
    s_state = ST_ERROR;
}

// "v1.2.3" / "1.2.3-x" -> comparable number; parts above 999 are clamped
static long version_num(const char *v)
{
    if (*v == 'v' || *v == 'V')
        v++;
    long n = 0;
    for (int i = 0; i < 3; i++) {
        long part = strtol(v, (char **)&v, 10);
        n = n * 1000 + (part < 0 ? 0 : part > 999 ? 999 : part);
        if (*v == '.')
            v++;
    }
    return n;
}

static void check_task(void *arg)
{
    char *json = malloc(MAX_RELEASE_JSON + 1);
    esp_http_client_config_t cfg = {
        .url = API_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = USER_AGENT,
        .timeout_ms = 15000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t c = json ? esp_http_client_init(&cfg) : NULL;
    int len = 0, status = 0;
    if (!c) {
        fail("out of memory%s", NULL);
    } else {
        esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
        if (esp_http_client_open(c, 0) != ESP_OK) {
            fail("cannot reach GitHub%s", NULL);
        } else {
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
            int k;
            while (len < MAX_RELEASE_JSON && (k = esp_http_client_read(c, json + len, MAX_RELEASE_JSON - len)) > 0)
                len += k;
            if (status != 200)
                fail("GitHub answered %s", status == 404 ? "404 (no release yet)" : "with an error");
        }
        esp_http_client_cleanup(c);
    }

    if (s_state == ST_CHECKING) {
        json[len] = 0;
        cJSON *root = cJSON_Parse(json);
        const cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
        const char *url = NULL;
        const cJSON *a;
        cJSON_ArrayForEach(a, cJSON_GetObjectItem(root, "assets"))
        {
            const cJSON *name = cJSON_GetObjectItem(a, "name");
            size_t nl = cJSON_IsString(name) ? strlen(name->valuestring) : 0;
            if (nl > strlen(ASSET_SUFFIX) && !strcmp(name->valuestring + nl - strlen(ASSET_SUFFIX), ASSET_SUFFIX)) {
                const cJSON *u = cJSON_GetObjectItem(a, "browser_download_url");
                url = cJSON_IsString(u) ? u->valuestring : NULL;
            }
        }
        if (!cJSON_IsString(tag)) {
            fail("unexpected answer from GitHub%s", NULL);
        } else {
            const char *t = tag->valuestring;
            snprintf(s_latest, sizeof(s_latest), "%s", (*t == 'v' || *t == 'V') ? t + 1 : t);
            if (version_num(s_latest) <= version_num(esp_app_get_description()->version)) {
                s_state = ST_CURRENT;
            } else if (!url || strlen(url) >= sizeof(s_url)) {
                fail("release %s has no *" ASSET_SUFFIX " file", t);
            } else {
                strcpy(s_url, url);
                s_state = ST_AVAILABLE;
            }
            ESP_LOGI(TAG, "latest release %s, running %s", s_latest, esp_app_get_description()->version);
        }
        cJSON_Delete(root);
    }
    free(json);
    release();
    vTaskDelete(NULL);
}

static void install_task(void *arg)
{
    esp_http_client_config_t http = {
        .url = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = USER_AGENT,
        .timeout_ms = 30000,
        .buffer_size = 4096,        // GitHub redirects to a long signed URL
        .buffer_size_tx = 2048,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t oc = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;
    ESP_LOGW(TAG, "installing release %s", s_latest);
    if (esp_https_ota_begin(&oc, &h) != ESP_OK) {
        fail("cannot download the firmware%s", NULL);
        h = NULL;
    } else {
        esp_app_desc_t d;
        if (esp_https_ota_get_img_desc(h, &d) != ESP_OK ||
            strncmp(d.project_name, esp_app_get_description()->project_name, sizeof(d.project_name)) != 0) {
            fail("the release file is not firmware for this board%s", NULL);
        } else {
            esp_err_t e;
            int size = esp_https_ota_get_image_size(h);
            while ((e = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
                int got = esp_https_ota_get_image_len_read(h);
                if (size > 0)
                    s_progress = (int)((int64_t)got * 100 / size);
            }
            if (e != ESP_OK || !esp_https_ota_is_complete_data_received(h))
                fail("download interrupted%s", NULL);
        }
    }
    if (h && s_state == ST_DOWNLOADING) {
        esp_err_t e = esp_https_ota_finish(h);      // also selects the new slot
        if (e == ESP_OK) {
            s_progress = 100;
            s_state = ST_DONE;
            ESP_LOGW(TAG, "release %s installed, restarting", s_latest);
            vTaskDelay(pdMS_TO_TICKS(3000));        // let the page see "done"
            esp_restart();
        }
        fail(e == ESP_ERR_OTA_VALIDATE_FAILED ? "the downloaded image is corrupted%s" : "update failed%s", NULL);
    } else if (h) {
        esp_https_ota_abort(h);
    }
    release();
    vTaskDelete(NULL);
}

static bool start(void (*fn)(void *), const char *name, int state)
{
    if (!claim())
        return false;
    s_err[0] = 0;
    s_progress = 0;
    s_state = state;
    if (xTaskCreate(fn, name, 8192, NULL, 4, NULL) != pdPASS) {
        fail("out of memory%s", NULL);
        release();
        return false;
    }
    return true;
}

bool fw_update_check_start(void)
{
    return start(check_task, "fw_check", ST_CHECKING);
}

bool fw_update_install_start(void)
{
    if (s_state != ST_AVAILABLE)
        return false;
    return start(install_task, "fw_install", ST_DOWNLOADING);
}

void fw_update_status_json(char *buf, size_t n)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", STATE_NAMES[s_state]);
    cJSON_AddStringToObject(o, "current", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "latest", s_latest);
    cJSON_AddNumberToObject(o, "progress", s_progress);
    cJSON_AddStringToObject(o, "error", s_err);
    cJSON_AddBoolToObject(o, "busy", usb_printer_job_active() || atomic_load(&s_running));
    if (!cJSON_PrintPreallocated(o, buf, (int)n, false))
        snprintf(buf, n, "{\"state\":\"error\",\"error\":\"status too long\"}");
    cJSON_Delete(o);
}
