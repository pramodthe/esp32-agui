// net_prov captive portal (P0.5) — SoftAP "AMOLED-setup" + HTTP setup form + DNS
// catch-all so phones auto-open the page. User submits SSID/pass → saved to NVS.

#include "net_prov.h"
#include "app_cfg.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "tz_options.h"   // auto-generated <option> list: IANA label -> POSIX TZ value
#include "alarm_img.h"    // configurable alarm graphic stored in flash (POST /alarmimg)

static const char *TAG = "net_portal";

#define PBIT_SAVED BIT0
#define PORTAL_IP  "192.168.4.1"

static httpd_handle_t     s_httpd;
static esp_netif_t       *s_ap_netif;
static EventGroupHandle_t s_portal_eg;
static TaskHandle_t       s_dns_task;
static volatile bool      s_dns_run;

// The page is sent in three chunks: HEAD + the (~24 KB) TZ_OPTIONS + TAIL. The timezone <select>
// carries the IANA name as the label and the POSIX TZ string as the option value, so the device
// just stores the submitted value (no on-device IANA table). A tiny script pre-selects the phone's
// own timezone via Intl, so the user normally doesn't touch it.
static const char FORM_HEAD[] =
    "<!doctype html><html><head><meta name=viewport "
    "content='width=device-width,initial-scale=1'><title>AMOLED setup</title>"
    "<style>body{font-family:sans-serif;margin:2em;max-width:30em}"
    "input,select{display:block;width:100%;padding:.6em;margin:.4em 0;font-size:1em}"
    "button{padding:.7em 1.2em;font-size:1em}</style></head><body>"
    "<h2>AG-UI device setup</h2>"
    "<form method=POST action=/save>"
    "<label>WiFi network (SSID)</label><input name=ssid autofocus>"
    "<label>WiFi password</label><input name=pass type=password>"
    "<label>Soniox API key</label><input name=soniox>"
    "<label>AG-UI endpoint URL</label><input name=agui_url>"
    "<label>AG-UI bearer token (optional)</label><input name=agui_token>"
    "<label>Timezone</label><select name=tz>";
// ... TZ_OPTIONS injected here ...
// Closes the timezone <select> and opens the TTS-voice <select>. The voice <option>s are emitted
// between this and FORM_TAIL by root_get(), with the saved voice marked selected.
static const char FORM_MID[] =
    "</select>"
    "<label>TTS voice</label><select name=voice>";

// Soniox tts-rt-v1 voices (https://soniox.com/docs/tts/concepts/voices). Adrian first = the default.
static const char *const TTS_VOICES[] = {
    "Adrian", "Maya", "Daniel", "Noah", "Nina", "Emma", "Jack", "Claire", "Grace", "Owen",
    "Mina", "Kenji", "Rafael", "Mateo", "Lucia", "Sofia", "Oliver", "Arthur", "Isla", "Victoria",
    "Cooper", "Mason", "Ruby", "Elise", "Arjun", "Rohan", "Priya", "Meera",
};
#define TTS_VOICE_DEFAULT "Adrian"

static const char FORM_TAIL[] =
    // NB: the voice <select> is closed by root_get's dynamic chunk (which also emits the screen-timeout
    // field pre-filled with the saved value), so FORM_TAIL no longer opens with "</select>".
    "<p style='color:#666;font-size:.85em'>Timezone is auto-detected from your phone; change it if "
    "needed. Leave WiFi blank if already connected; fill the Soniox key to enable voice and the "
    "AG-UI URL to enable the agent.</p>"
    "<button type=submit>Save &amp; connect</button></form>"
    "<script>try{var z=Intl.DateTimeFormat().resolvedOptions().timeZone,"
    "s=document.querySelector('select[name=tz]');"
    "for(var i=0;i<s.options.length;i++){if(s.options[i].text===z){s.selectedIndex=i;break;}}}"
    "catch(e){}</script>"
    // Alarm graphic: any image, cropped in-browser to 240x240 and converted to RGB565 (high byte
    // first, matching the device's LV_COLOR_16_SWAP) so the device stores the raw bytes with no decode.
    "<hr><h3>Alarm image (optional)</h3>"
    "<p style='color:#666;font-size:.85em'>Shown when a timer goes off. Any image - it's cropped to "
    "a 240x240 square (preview below) and sent to the device.</p>"
    "<input type=file id=aimg accept='image/*'>"
    "<canvas id=acv width=240 height=240 style='width:120px;height:120px;border:1px solid #ccc;"
    "background:#000;display:block;margin:.4em 0'></canvas>"
    "<button id=asend type=button disabled>Upload alarm image</button>"
    "<span id=astat style='margin-left:.6em'></span>"
    "<script>(function(){"
    "var f=document.getElementById('aimg'),cv=document.getElementById('acv'),"
    "b=document.getElementById('asend'),st=document.getElementById('astat'),W=240,H=240;"
    "f.onchange=function(){var fi=f.files[0];if(!fi)return;var im=new Image();"
    "im.onload=function(){var c=cv.getContext('2d'),s=Math.max(W/im.width,H/im.height),"
    "w=im.width*s,h=im.height*s;c.fillStyle='#000';c.fillRect(0,0,W,H);"
    "c.drawImage(im,(W-w)/2,(H-h)/2,w,h);b.disabled=false;st.textContent='';"
    "URL.revokeObjectURL(im.src);};im.src=URL.createObjectURL(fi);};"
    "b.onclick=function(){var c=cv.getContext('2d'),d=c.getImageData(0,0,W,H).data,"
    "o=new Uint8Array(W*H*2),j=0,i,v;"
    "for(i=0;i<d.length;i+=4){v=((d[i]>>3)<<11)|((d[i+1]>>2)<<5)|(d[i+2]>>3);"
    "o[j++]=v>>8;o[j++]=v&255;}"
    "b.disabled=true;st.textContent='Uploading...';"
    "fetch('/alarmimg',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:o})"
    ".then(function(r){st.textContent=r.ok?'Saved':'Failed';b.disabled=false;})"
    ".catch(function(){st.textContent='Failed';b.disabled=false;});};"
    "})();</script></body></html>";

// ---- tiny form helpers ---------------------------------------------------

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// URL-decode src into dst (handles %xx and '+').
static void url_decode(const char *src, char *dst, size_t dstsz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dstsz; i++) {
        if (src[i] == '+') {
            dst[o++] = ' ';
        } else if (src[i] == '%' && hexv(src[i + 1]) >= 0 && hexv(src[i + 2]) >= 0) {
            dst[o++] = (char)(hexv(src[i + 1]) * 16 + hexv(src[i + 2]));
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

// Extract field `name` from urlencoded body into out (decoded). Returns true if found.
static bool form_field(const char *body, const char *name, char *out, size_t outsz)
{
    char key[24];
    int kl = snprintf(key, sizeof(key), "%s=", name);
    const char *p = body;
    while ((p = strstr(p, key))) {
        if (p == body || p[-1] == '&') {     // key at start of a pair
            p += kl;
            const char *end = strchr(p, '&');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            char raw[256];
            if (len >= sizeof(raw)) len = sizeof(raw) - 1;
            memcpy(raw, p, len);
            raw[len] = '\0';
            url_decode(raw, out, outsz);
            return true;
        }
        p += kl;
    }
    return false;
}

// ---- HTTP handlers -------------------------------------------------------

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");   // always serve the current form (no stale cache)
    httpd_resp_send_chunk(req, FORM_HEAD, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, TZ_OPTIONS, HTTPD_RESP_USE_STRLEN);   // ~24 KB timezone <option> list
    httpd_resp_send_chunk(req, FORM_MID, HTTPD_RESP_USE_STRLEN);     // close tz <select>, open voice <select>

    // TTS-voice <option>s, pre-selecting the saved voice (so re-saving keeps it); default Adrian.
    char saved[APP_CFG_VAL_MAX];
    if (!app_cfg_get(APP_CFG_TTS_VOICE, saved, sizeof saved)) strlcpy(saved, TTS_VOICE_DEFAULT, sizeof saved);
    char opts[1500];
    size_t o = 0;
    for (size_t i = 0; i < sizeof(TTS_VOICES) / sizeof(TTS_VOICES[0]); i++)
        o += snprintf(opts + o, sizeof(opts) - o, "<option%s>%s</option>",
                      strcmp(TTS_VOICES[i], saved) == 0 ? " selected" : "", TTS_VOICES[i]);

    // Close the voice <select>, then the screen-timeout field, pre-filled with the saved value (so
    // re-saving keeps it). Default 60 s; 0 = always on.
    char scr_to[8];
    if (!app_cfg_get(APP_CFG_SCREEN_TO, scr_to, sizeof scr_to)) strlcpy(scr_to, "60", sizeof scr_to);
    o += snprintf(opts + o, sizeof(opts) - o,
                  "</select>"
                  "<label>Screen blank timeout (seconds, 0 = always on)</label>"
                  "<input name=scr_to type=number min=0 max=86400 value='%s'>", scr_to);

    // Idle-animation checkbox, pre-checked from the saved state (so re-saving keeps it). Inline style
    // overrides the form's block/full-width input rule.
    char ia[4];
    bool ia_on = app_cfg_get(APP_CFG_IDLE_ANIM, ia, sizeof ia) && ia[0] == '1';
    o += snprintf(opts + o, sizeof(opts) - o,
                  "<label style='display:flex;align-items:center;gap:.5em;margin:.5em 0'>"
                  "<input type=checkbox name=idle_anim value=1%s style='width:auto;display:inline;margin:0'>"
                  " Idle animation (gently pulse the uploaded alarm image when idle)</label>",
                  ia_on ? " checked" : "");
    httpd_resp_send_chunk(req, opts, o);

    httpd_resp_send_chunk(req, FORM_TAIL, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);              // terminate the chunked response
}

static esp_err_t save_post(httpd_req_t *req)
{
    static char body[2048];   // urlencoded fields (now incl. tz); static (httpd is single-threaded) keeps it off the stack
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = '\0';

    char ssid[33] = {0}, pass[65] = {0}, soniox[APP_CFG_VAL_MAX] = {0};
    char agui_url[APP_CFG_VAL_MAX] = {0}, agui_token[APP_CFG_VAL_MAX] = {0}, tz[64] = {0};
    char voice[APP_CFG_VAL_MAX] = {0}, scr_to[8] = {0};
    bool have_ssid   = form_field(body, "ssid", ssid, sizeof(ssid)) && ssid[0];
    bool have_soniox = form_field(body, "soniox", soniox, sizeof(soniox)) && soniox[0];
    bool have_url    = form_field(body, "agui_url", agui_url, sizeof(agui_url)) && agui_url[0];
    bool have_token  = form_field(body, "agui_token", agui_token, sizeof(agui_token)) && agui_token[0];
    bool have_tz     = form_field(body, "tz", tz, sizeof(tz)) && tz[0];
    bool have_voice  = form_field(body, "voice", voice, sizeof(voice)) && voice[0];
    bool have_scr    = form_field(body, "scr_to", scr_to, sizeof(scr_to)) && scr_to[0];
    ESP_LOGI(TAG, "save: body=%dB  ssid=%d soniox=%d url=%d token=%d tz=%d voice=%d scr=%d",
             got, have_ssid, have_soniox, have_url, have_token, have_tz, have_voice, have_scr);
    if (!have_ssid && !have_soniox && !have_url && !have_token && !have_tz && !have_voice && !have_scr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nothing to save");
        return ESP_FAIL;
    }
    if (have_ssid) {
        form_field(body, "pass", pass, sizeof(pass));
        net_creds_add(ssid, pass);
    }
    if (have_soniox) app_cfg_set(APP_CFG_SONIOX_KEY, soniox);
    if (have_url)    app_cfg_set(APP_CFG_AGUI_URL, agui_url);
    if (have_token)  app_cfg_set(APP_CFG_AGUI_TOKEN, agui_token);
    if (have_tz)     app_cfg_set(APP_CFG_TZ, tz);
    if (have_voice)  app_cfg_set(APP_CFG_TTS_VOICE, voice);
    if (have_scr) {                                  // clamp to [0, 86400] s, store the clamped value
        int n = atoi(scr_to);
        if (n < 0) n = 0;
        if (n > 86400) n = 86400;
        snprintf(scr_to, sizeof scr_to, "%d", n);
        app_cfg_set(APP_CFG_SCREEN_TO, scr_to);
    }
    // Idle-animation checkbox: a checkbox is absent from the body when unchecked. Trust it only when
    // our current form was submitted (scr_to is always in it) so a stale/foreign POST can't flip it.
    bool ia_on = false;
    if (have_scr) {
        char tmp[4] = {0};
        ia_on = form_field(body, "idle_anim", tmp, sizeof tmp);
        app_cfg_set(APP_CFG_IDLE_ANIM, ia_on ? "1" : "0");
    }

    char scr_disp[16];
    if (!have_scr)                strlcpy(scr_disp, "unchanged", sizeof scr_disp);
    else if (atoi(scr_to) == 0)   strlcpy(scr_disp, "always on", sizeof scr_disp);
    else                          snprintf(scr_disp, sizeof scr_disp, "%s s", scr_to);
    const char *ia_disp = have_scr ? (ia_on ? "on" : "off") : "unchanged";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    static char resp[1024];
    snprintf(resp, sizeof resp,
        "<html><head><meta name=viewport content='width=device-width,initial-scale=1'></head>"
        "<body style='font-family:sans-serif;margin:2em'><h3>Saved</h3>"
        "<p>This is exactly what the device received:</p><ul>"
        "<li>WiFi: <b>%s</b></li>"
        "<li>Soniox key: <b>%s</b></li>"
        "<li>AG-UI URL: <b>%s</b></li>"
        "<li>AG-UI token: <b>%s</b></li>"
        "<li>Timezone: <b>%s</b></li>"
        "<li>TTS voice: <b>%s</b></li>"
        "<li>Screen timeout: <b>%s</b></li>"
        "<li>Idle animation: <b>%s</b></li>"
        "</ul><p>If AG-UI URL says \"unchanged\" but you typed one, your phone submitted a cached "
        "form — reload <a href='http://192.168.4.1/'>192.168.4.1</a> and try again.</p></body></html>",
        have_ssid ? "updated" : "unchanged",
        have_soniox ? "updated" : "unchanged",
        have_url ? agui_url : "unchanged",
        have_token ? "updated" : "unchanged",
        have_tz ? tz : "unchanged",
        have_voice ? voice : "unchanged",
        scr_disp, ia_disp);
    httpd_resp_sendstr(req, resp);
    xEventGroupSetBits(s_portal_eg, PBIT_SAVED);
    return ESP_OK;
}

// Binary upload of the alarm graphic: exactly ALARM_IMG_BYTES of RGB565 (the browser crops + converts).
// Stream the body into a PSRAM buffer (keeps the tight internal heap free), then commit to flash.
static esp_err_t alarmimg_post(httpd_req_t *req)
{
    if (req->content_len != ALARM_IMG_BYTES) {
        ESP_LOGW(TAG, "alarmimg: wrong size %d (want %d)", req->content_len, (int)ALARM_IMG_BYTES);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wrong image size");
        return ESP_FAIL;
    }
    uint8_t *buf = heap_caps_malloc(ALARM_IMG_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_FAIL; }

    int got = 0;
    while (got < ALARM_IMG_BYTES) {
        int r = httpd_req_recv(req, (char *)buf + got, ALARM_IMG_BYTES - got);
        if (r <= 0) { heap_caps_free(buf); return ESP_FAIL; }
        got += r;
    }
    esp_err_t e = alarm_img_write(buf, got);
    heap_caps_free(buf);
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "alarm image saved (%d B)", got);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

// Catch-all: redirect every other request to the portal (triggers OS captive detection).
static esp_err_t redirect_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

// ---- minimal DNS catch-all (answers every A query with the portal IP) ----

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(sock); vTaskDelete(NULL); return;
    }
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&cli, &cl);
        if (n < (int)sizeof(uint16_t) * 6) continue;   // not a sane DNS query
        // Build a response in place: flags = standard response, 1 answer.
        buf[2] = 0x81; buf[3] = 0x80;          // QR=1, RD=1, RA=1
        buf[6] = 0x00; buf[7] = 0x01;          // ANCOUNT = 1
        if (n + 16 > (int)sizeof(buf)) continue;
        uint8_t *a = buf + n;
        *a++ = 0xC0; *a++ = 0x0C;              // name pointer to question
        *a++ = 0x00; *a++ = 0x01;              // type A
        *a++ = 0x00; *a++ = 0x01;              // class IN
        *a++ = 0x00; *a++ = 0x00; *a++ = 0x00; *a++ = 0x3C;  // TTL 60
        *a++ = 0x00; *a++ = 0x04;              // RDLENGTH 4
        *a++ = 192; *a++ = 168; *a++ = 4; *a++ = 1;          // 192.168.4.1
        sendto(sock, buf, a - buf, 0, (struct sockaddr *)&cli, cl);
    }
    close(sock);
    vTaskDelete(NULL);
}

// ---- public API ----------------------------------------------------------

esp_err_t net_portal_start(const char *ap_ssid)
{
    if (!ap_ssid) ap_ssid = "AMOLED-setup";
    if (!s_portal_eg) s_portal_eg = xEventGroupCreate();
    xEventGroupClearBits(s_portal_eg, PBIT_SAVED);

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;   // save_post handles ~2 KB of buffers; the 4 KB default overflows
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) return ESP_FAIL;
    httpd_uri_t u_root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    httpd_uri_t u_save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_uri_t u_aimg = { .uri = "/alarmimg", .method = HTTP_POST, .handler = alarmimg_post };
    httpd_uri_t u_any  = { .uri = "/*", .method = HTTP_GET, .handler = redirect_get };
    httpd_register_uri_handler(s_httpd, &u_root);
    httpd_register_uri_handler(s_httpd, &u_save);
    httpd_register_uri_handler(s_httpd, &u_aimg);
    httpd_register_uri_handler(s_httpd, &u_any);

    s_dns_run = true;
    xTaskCreate(dns_task, "portal_dns", 3072, NULL, 5, &s_dns_task);

    ESP_LOGI(TAG, "captive portal up: SSID '%s' → http://%s/", ap_ssid, PORTAL_IP);
    return ESP_OK;
}

bool net_portal_wait_saved(uint32_t timeout_ms)
{
    if (!s_portal_eg) return false;
    TickType_t to = timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t b = xEventGroupWaitBits(s_portal_eg, PBIT_SAVED, pdTRUE, pdFALSE, to);
    return (b & PBIT_SAVED) != 0;
}

void net_portal_stop(void)
{
    s_dns_run = false;
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    // Give the DNS task a tick to exit its 1s-timeout recv and self-delete.
    vTaskDelay(pdMS_TO_TICKS(1100));
    s_dns_task = NULL;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "captive portal down");
}
