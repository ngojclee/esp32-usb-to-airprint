#include "setup_web.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "setup";

// ---------------------------------------------------------------------------
// small helpers

typedef struct {
    char *p;
    size_t n, cap;
} sb_t;

static void sb_add(sb_t *b, const char *fmt, ...)
{
    if (!b->p)
        return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (k > 0)
        b->n = b->n + (size_t)k < b->cap ? b->n + (size_t)k : b->cap - 1;
}

// HTML-escaped copy of s
static void sb_esc(sb_t *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': sb_add(b, "&amp;"); break;
        case '<': sb_add(b, "&lt;"); break;
        case '>': sb_add(b, "&gt;"); break;
        case '"': sb_add(b, "&quot;"); break;
        case '\'': sb_add(b, "&#39;"); break;
        default: sb_add(b, "%c", *s); break;
        }
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Value of `key` in an x-www-form-urlencoded body, decoded into out.
// false if the key is absent.
static bool form_get(const char *body, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = body; p && *p;) {
        const char *end = strchr(p, '&');
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t w = 0;
            while (v < (end ? end : v + strlen(v)) && w + 1 < n) {
                if (*v == '+') {
                    out[w++] = ' ';
                    v++;
                } else if (*v == '%' && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                    out[w++] = (char)(hexval(v[1]) * 16 + hexval(v[2]));
                    v += 3;
                } else {
                    out[w++] = *v++;
                }
            }
            out[w] = 0;
            return true;
        }
        p = end ? end + 1 : NULL;
    }
    if (n)
        out[0] = 0;
    return false;
}

static void restart_cb(void *arg)
{
    esp_restart();
}

static void restart_soon(void)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = {.callback = restart_cb, .name = "restart"};
    if (!t)
        esp_timer_create(&a, &t);
    esp_timer_start_once(t, 1500 * 1000);
}

// ---------------------------------------------------------------------------
// pages

static const char CSS[] =
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>:root{color-scheme:light dark;--bg:#f6f6f4;--fg:#1b1b1a;--card:#fff;--line:#d9d9d4;--acc:#2457c5}"
    "@media(prefers-color-scheme:dark){:root{--bg:#161615;--fg:#ececea;--card:#22221f;--line:#3a3a36;--acc:#7aa2ff}}"
    "body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,sans-serif}"
    "main{max-width:520px;margin:0 auto;padding:20px 16px}h1{font-size:1.4em;margin:.2em 0 .8em}"
    "fieldset{background:var(--card);border:1px solid var(--line);border-radius:10px;margin:0 0 16px;padding:12px 14px}"
    "legend{font-weight:600;padding:0 4px}label{display:block;margin:8px 0 2px;font-size:.92em}"
    "select,input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:9px;border:1px solid var(--line);"
    "border-radius:7px;background:var(--bg);color:var(--fg);font-size:16px}"
    ".row{display:flex;gap:14px;margin:6px 0}.row label{display:inline;margin:0}#ssid{margin-top:8px}"
    "button{font-size:16px;padding:10px 16px;border-radius:8px;border:0;background:var(--acc);color:#fff;cursor:pointer}"
    "button.danger{background:transparent;color:#c33;border:1px solid #c33}small{opacity:.75}"
    ".note{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px}</style>";

static void send_page(airprint_http_t *h, const char *status, sb_t *b)
{
    airprint_http_send_ex(h, status, "text/html; charset=utf-8", "Cache-Control: no-store\r\n", b->p, b->n);
    free(b->p);
}

static void message_page(airprint_http_t *h, const char *status, const char *title, const char *html_body)
{
    sb_t b = {malloc(4096), 0, 4096};
    sb_add(&b, "<!doctype html><title>%s</title>%s<main><h1>%s</h1><div class=note>%s</div>"
               "<p><a href='/setup'>Back to settings</a></p></main>",
           title, CSS, title, html_body);
    send_page(h, status, &b);
}

// ---------------------------------------------------------------------------
// login: with a settings password set, /setup asks for it once, then a
// session cookie (one at a time, in RAM, 30 min idle) lets the browser in.

#define SESSION_IDLE_US (30LL * 60 * 1000 * 1000)

static char s_sid[33];          // hex, "" = nobody logged in
static int64_t s_sid_until;
static int s_failures;          // wrong passwords in a row

static bool same_secret(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < la && i < lb; i++)
        diff |= (unsigned)(a[i] ^ b[i]);
    return diff == 0;
}

static bool logged_in(airprint_http_t *h)
{
    if (!settings_get()->admin[0])
        return true;            // no password set: open, as on first setup
    if (!s_sid[0] || esp_timer_get_time() > s_sid_until)
        return false;
    const char *c = airprint_http_cookie(h);
    const char *p = strstr(c, "psid=");
    if (!p || (p != c && p[-1] != ' ' && p[-1] != ';'))
        return false;
    char got[sizeof(s_sid)];
    size_t n = strcspn(p + 5, "; ");
    if (n != strlen(s_sid))
        return false;
    memcpy(got, p + 5, n);
    got[n] = 0;
    if (!same_secret(got, s_sid))
        return false;
    s_sid_until = esp_timer_get_time() + SESSION_IDLE_US;
    return true;
}

static void login_page(airprint_http_t *h, const char *status, const char *note)
{
    sb_t b = {malloc(4096), 0, 4096};
    sb_add(&b, "<!doctype html><title>Printer setup</title>%s<main><h1>Printer setup</h1>"
               "<form method=post action='/setup'><fieldset><legend>Log in</legend>%s%s%s"
               "<label for=password>Settings password</label>"
               "<input type=password id=password name=password autocomplete=current-password autofocus>"
               "<input type=hidden name=action value=login></fieldset>"
               "<button type=submit>Log in</button></form>"
               "<p><small>Forgot it? Hold the BOOT button on the board for 10 seconds: all settings are "
               "erased and the printer starts its setup WiFi \"%s\".</small></p></main>",
           CSS, note ? "<p>" : "", note ? note : "", note ? "</p>" : "", wifi_setup_ap_name());
    send_page(h, status, &b);
}

static void login(airprint_http_t *h, const char *pw)
{
    // a little slower after every wrong guess (up to 5 s)
    if (s_failures)
        vTaskDelay(pdMS_TO_TICKS(s_failures < 5 ? s_failures * 1000 : 5000));
    if (!settings_get()->admin[0] || !same_secret(pw, settings_get()->admin)) {
        s_failures++;
        ESP_LOGW(TAG, "wrong settings password (%d in a row)", s_failures);
        login_page(h, "403 Forbidden", "Wrong password.");
        return;
    }
    s_failures = 0;
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++)
        snprintf(s_sid + 2 * i, 3, "%02x", rnd[i]);
    s_sid_until = esp_timer_get_time() + SESSION_IDLE_US;
    char hdr[160];
    snprintf(hdr, sizeof(hdr), "Location: /setup\r\nSet-Cookie: psid=%s; Path=/setup; HttpOnly; SameSite=Strict\r\n",
             s_sid);
    airprint_http_send_ex(h, "303 See Other", "text/html", hdr, "", 0);
}

static void setup_get(airprint_http_t *h)
{
    if (!logged_in(h)) {
        login_page(h, "200 OK", NULL);
        return;
    }
    const settings_t *s = settings_get();
    wifi_net_t nets[15];
    int nnet = wifi_scan(nets, 15);

    sb_t b = {malloc(12 * 1024), 0, 12 * 1024};
    sb_add(&b, "<!doctype html><title>Printer setup</title>%s<main><h1>Printer setup</h1>", CSS);
    char ip[16];
    if (wifi_get_ip(ip, sizeof(ip)))
        sb_add(&b, "<p><small>Connected, IP %s</small></p>", ip);
    else if (wifi_setup_ap_active())
        sb_add(&b, "<p><small>Not connected to a network yet.</small></p>");

    // Networks in range as a drop-down (strongest first, the saved one on
    // top), "Other network..." for a hidden or out of range one. Without
    // JavaScript the plain text box below does the job.
    sb_add(&b, "<form method=post action='/setup' autocomplete=off><fieldset><legend>WiFi</legend>"
               "<label for=netsel>Network</label><div class=row style='margin:0'>"
               "<select id=netsel hidden style='flex:1'>");
    bool saved_in_range = false;
    for (int i = 0; i < nnet; i++)
        saved_in_range |= !strcmp(nets[i].ssid, s->ssid);
    if (s->ssid[0] && !saved_in_range) {
        sb_add(&b, "<option value=\"");
        sb_esc(&b, s->ssid);
        sb_add(&b, "\" selected>");
        sb_esc(&b, s->ssid);
        sb_add(&b, " (saved, not in range)</option>");
    }
    for (int i = 0; i < nnet; i++) {
        const char *bars = nets[i].rssi >= -55 ? "▂▄▆█" : nets[i].rssi >= -67 ? "▂▄▆"
                         : nets[i].rssi >= -78 ? "▂▄" : "▂";
        sb_add(&b, "<option value=\"");
        sb_esc(&b, nets[i].ssid);
        sb_add(&b, "\"%s>", !strcmp(nets[i].ssid, s->ssid) ? " selected" : "");
        sb_esc(&b, nets[i].ssid);
        sb_add(&b, " &nbsp;%s%s</option>", bars, nets[i].secure ? " \U0001F512" : "");
    }
    sb_add(&b, "<option value=''%s>Other network…</option></select>"
               "<a href='/setup' style='align-self:center'>Rescan</a></div>"
               "<input type=text id=ssid name=ssid maxlength=32 placeholder='Network name' value=\"",
           s->ssid[0] ? "" : " selected");
    sb_esc(&b, s->ssid);
    // readonly until focused: keeps the browser from filling in a saved
    // web password, which would silently replace the WiFi password
    sb_add(&b, "\"><label for=pass>Password</label>"
               "<input type=password id=pass name=pass placeholder='%s' autocomplete=off readonly "
               "onfocus=\"this.removeAttribute('readonly')\"></fieldset>"
               "<script>(function(){var s=document.getElementById('netsel'),t=document.getElementById('ssid');"
               "s.hidden=false;function u(){if(s.value){t.value=s.value;t.hidden=true}else{t.hidden=false;"
               "if(t.value===s.options[0].value)t.value=''}}s.onchange=function(){u();if(!s.value)t.focus()};"
               "u()})()</script>",
           s->pass[0] ? "unchanged" : "none");

    sb_add(&b, "<fieldset><legend>Printer</legend><label for=name>Name shown on phones and computers (empty: the printer model)</label>"
               "<input type=text id=name name=name maxlength=63 value=\"");
    sb_esc(&b, s->name);
    sb_add(&b, "\"><label for=host>Network name (<i>name</i>.local), a-z 0-9 -</label>"
               "<input type=text id=host name=hostname maxlength=32 value=\"");
    sb_esc(&b, s->hostname);
    sb_add(&b, "\"></fieldset>");

    sb_add(&b, "<fieldset><legend>IP address</legend><div class=row>"
               "<label><input type=radio name=ipmode value=dhcp%s> Automatic (DHCP)</label>"
               "<label><input type=radio name=ipmode value=static%s> Fixed</label></div>",
           s->static_ip ? "" : " checked", s->static_ip ? " checked" : "");
    static const char *const F[4][2] = {{"ip", "IP address"}, {"mask", "Subnet mask"},
                                        {"gw", "Gateway"}, {"dns", "DNS (empty: the gateway)"}};
    const char *V[4] = {s->ip, s->mask[0] ? s->mask : "255.255.255.0", s->gw, s->dns};
    sb_add(&b, "<div id=fixed>");
    for (int i = 0; i < 4; i++) {
        sb_add(&b, "<label for=%s>%s</label><input type=text id=%s name=%s inputmode=decimal value=\"", F[i][0],
               F[i][1], F[i][0], F[i][0]);
        sb_esc(&b, V[i]);
        sb_add(&b, "\">");
    }
    sb_add(&b, "</div></fieldset><script>(function(){var f=document.getElementById('fixed'),"
               "r=document.querySelectorAll('input[name=ipmode]');function u(){f.hidden=!r[1].checked}"
               "r.forEach(function(e){e.onchange=u});u()})()</script>");

    // Only to set or change it; the page itself is behind the login already.
    sb_add(&b, "<fieldset><legend>Settings password</legend>"
               "<p><small>%s</small></p>"
               "<label for=admin_new>New password</label>"
               "<input type=password id=admin_new name=admin_new autocomplete=new-password>"
               "<label for=admin_rep>New password again</label>"
               "<input type=password id=admin_rep name=admin_rep autocomplete=new-password>",
           s->admin[0] ? "Leave empty to keep the current one." : "None yet: anyone on the network can open this page.");
    if (s->admin[0])
        sb_add(&b, "<label for=admin>Current password (only to change it)</label>"
                   "<input type=password id=admin name=admin autocomplete=current-password>");
    sb_add(&b, "</fieldset><button type=submit>Save and restart</button></form>");

    sb_add(&b, "<form method=post action='/setup' style='margin-top:28px'><input type=hidden name=action value=reset>"
               "<p><small>Forget all settings and restart in setup mode (WiFi \"%s\"). Holding the BOOT "
               "button for 10 seconds does the same.</small></p>"
               "<button class=danger type=submit onclick=\"return confirm('Erase all settings?')\">Factory reset"
               "</button></form>",
           wifi_setup_ap_name());
    if (s->admin[0])
        sb_add(&b, "<form method=post action='/setup' style='margin-top:20px'>"
                   "<input type=hidden name=action value=logout><button type=submit>Log out</button></form>");
    sb_add(&b, "</main>");
    send_page(h, "200 OK", &b);
}

static void setup_post(airprint_http_t *h)
{
    char *body = calloc(1, 2049);
    if (!body) {
        message_page(h, "500 Internal Server Error", "Error", "Out of memory.");
        return;
    }
    size_t n = 0;
    int k;
    while (n < 2048 && (k = airprint_http_read_body(h, body + n, 2048 - n)) > 0)
        n += (size_t)k;

    const settings_t *cur = settings_get();
    char action[16];
    form_get(body, "action", action, sizeof(action));

    if (!strcmp(action, "login")) {
        char pw[sizeof(cur->admin)];
        form_get(body, "password", pw, sizeof(pw));
        free(body);
        login(h, pw);
        memset(pw, 0, sizeof(pw));
        return;
    }
    if (!logged_in(h)) {
        free(body);
        login_page(h, "403 Forbidden", "Please log in first.");
        return;
    }
    if (!strcmp(action, "logout")) {
        free(body);
        s_sid[0] = 0;
        airprint_http_send_ex(h, "303 See Other", "text/html",
                              "Location: /setup\r\nSet-Cookie: psid=; Path=/setup; Max-Age=0\r\n", "", 0);
        return;
    }
    if (!strcmp(action, "reset")) {
        free(body);
        settings_erase();
        ESP_LOGW(TAG, "factory reset from the web page");
        char msg[256];
        snprintf(msg, sizeof(msg), "Settings erased. The printer restarts; join the WiFi <b>%s</b> to set it up again.",
                 wifi_setup_ap_name());
        message_page(h, "200 OK", "Factory reset", msg);
        restart_soon();
        return;
    }

    settings_t s = *cur;
    char pass[sizeof(s.pass)], admin_new[sizeof(s.admin)], ipmode[8];
    form_get(body, "ssid", s.ssid, sizeof(s.ssid));
    bool new_pass = form_get(body, "pass", pass, sizeof(pass)) && pass[0];
    if (new_pass)
        memcpy(s.pass, pass, sizeof(s.pass));
    else if (strcmp(s.ssid, cur->ssid) != 0)
        s.pass[0] = 0;              // another network without a password given: an open one
    form_get(body, "name", s.name, sizeof(s.name));
    form_get(body, "hostname", s.hostname, sizeof(s.hostname));
    form_get(body, "ipmode", ipmode, sizeof(ipmode));
    s.static_ip = !strcmp(ipmode, "static");
    form_get(body, "ip", s.ip, sizeof(s.ip));
    form_get(body, "mask", s.mask, sizeof(s.mask));
    form_get(body, "gw", s.gw, sizeof(s.gw));
    form_get(body, "dns", s.dns, sizeof(s.dns));
    // a new settings password: typed twice, and the current one once
    const char *err = NULL;
    char admin_rep[sizeof(s.admin)], admin_cur[sizeof(s.admin)];
    if (form_get(body, "admin_new", admin_new, sizeof(admin_new)) && admin_new[0]) {
        form_get(body, "admin_rep", admin_rep, sizeof(admin_rep));
        form_get(body, "admin", admin_cur, sizeof(admin_cur));
        if (strcmp(admin_new, admin_rep) != 0)
            err = "The two new passwords differ.";
        else if (cur->admin[0] && !same_secret(admin_cur, cur->admin))
            err = "The current password is not correct: the password was not changed.";
        else
            memcpy(s.admin, admin_new, sizeof(s.admin));
        memset(admin_rep, 0, sizeof(admin_rep));
        memset(admin_cur, 0, sizeof(admin_cur));
    }
    memset(body, 0, 2049);
    free(body);

    if (err)
        ;
    else if (!s.ssid[0])
        err = "Enter the WiFi network name.";
    else if (strlen(s.pass) > 0 && strlen(s.pass) < 8)
        err = "A WiFi password has at least 8 characters.";
    else if (!settings_hostname_ok(s.hostname))
        err = "The network name may only use a-z, 0-9 and '-' (up to 32).";
    else if (s.static_ip && (!settings_ipv4_ok(s.ip) || !settings_ipv4_ok(s.mask) || !settings_ipv4_ok(s.gw) ||
                             (s.dns[0] && !settings_ipv4_ok(s.dns))))
        err = "Fixed IP: enter IP address, subnet mask and gateway as a.b.c.d.";
    if (err) {
        message_page(h, "400 Bad Request", "Not saved", err);
        return;
    }
    if (settings_save(&s) != ESP_OK) {
        message_page(h, "500 Internal Server Error", "Error", "Could not save the settings.");
        return;
    }
    ESP_LOGI(TAG, "settings saved (host %s, %s), restarting", s.hostname, s.static_ip ? "static IP" : "DHCP");

    sb_t b = {malloc(2048), 0, 2048};
    sb_add(&b, "Saved. The printer restarts and joins <b>");
    sb_esc(&b, s.ssid);
    sb_add(&b, "</b>. Then open <a href='http://%s.local/'>http://%s.local/</a>", s.hostname, s.hostname);
    if (s.static_ip)
        sb_add(&b, " or <a href='http://%s/'>http://%s/</a>", s.ip, s.ip);
    sb_add(&b, ".<br><br>If it cannot join, the setup WiFi <b>%s</b> comes back after 2 minutes.",
           wifi_setup_ap_name());
    message_page(h, "200 OK", "Saved", b.p ? b.p : "Saved.");
    free(b.p);
    restart_soon();
}

// ---------------------------------------------------------------------------

// Requests to other hosts while the setup WiFi is up are a phone's captive
// portal check (or any site the user tried): send them to /setup.
static bool foreign_host(airprint_http_t *h)
{
    const char *host = airprint_http_host(h);
    size_t hl = strcspn(host, ":");
    if (!hl)
        return false;
    if (hl == strlen(WIFI_SETUP_IP) && !strncmp(host, WIFI_SETUP_IP, hl))
        return false;
    char ip[16];
    if (wifi_get_ip(ip, sizeof(ip)) && hl == strlen(ip) && !strncmp(host, ip, hl))
        return false;
    char local[48];
    snprintf(local, sizeof(local), "%s.local", settings_get()->hostname);
    if (hl == strlen(local) && !strncasecmp(host, local, hl))
        return false;
    return true;
}

bool setup_web_http(airprint_http_t *h, const char *method, const char *path)
{
    size_t pl = strcspn(path, "?#");
    bool get = !strcmp(method, "GET") || !strcmp(method, "HEAD");

    if (wifi_setup_ap_active() && get && (foreign_host(h) || (pl == 1 && path[0] == '/'))) {
        airprint_http_send_ex(h, "302 Found", "text/html", "Location: http://" WIFI_SETUP_IP "/setup\r\n", "", 0);
        return true;
    }
    if (pl != 6 || strncmp(path, "/setup", 6) != 0)
        return false;
    if (get) {
        setup_get(h);
        return true;
    }
    if (!strcmp(method, "POST")) {
        setup_post(h);
        return true;
    }
    return false;
}
