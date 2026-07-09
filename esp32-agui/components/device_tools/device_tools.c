#include "device_tools.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"
#include "app_cfg.h"

static const char *TAG = "device_tools";

static void register_builtins(void);   // defined with the tool registry (P7), below
static QueueHandle_t s_timer_q;        // signaled when a set_timer fires (lets the alert task block, not poll)

// Ambient context (P5): read-only device signals pushed to the agent each run via
// RunAgentInput.context. The AG-UI Context.value is a STRING, so structured signals are
// JSON-stringified into the value (see ctx_add). v1 ships current_local_time + battery (AXP2101);
// device_motion (QMI8658) is deferred (low value for a desktop device). set_timer/set_alarm/show_qr
// tools are P7.

esp_err_t device_tools_init(void)
{
    ESP_LOGI(TAG, "init");
    s_timer_q = xQueueCreate(1, sizeof(uint8_t));   // set_timer fire signal (block on it, don't poll)
    register_builtins();   // P7: register builtin client tools (set_timer, ...)
    return ESP_OK;
}

// Append one ambient-context entry {description, value}. `value` is always a string (the AG-UI
// Context.value type); pass a JSON-stringified object for structured signals.
static void ctx_add(cJSON *arr, const char *description, const char *value)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) return;
    cJSON_AddStringToObject(item, "description", description);
    cJSON_AddStringToObject(item, "value", value);
    cJSON_AddItemToArray(arr, item);
}

// local_time → ISO-8601 with numeric offset (e.g. 2026-06-23T13:00:00-0500; UTC → +0000). The TZ
// is applied at boot from the "tz" config (POSIX TZ string, default UTC0). Only emitted once the
// clock has been set — before that it's the 1970 boot epoch, and a wrong time is worse than none.
static void add_local_time(cJSON *arr)
{
    time_t now = time(NULL);
    if (now < 1700000000) return;   // ~2023-11; below this the clock hasn't synced yet
    struct tm lt;
    localtime_r(&now, &lt);
    char iso[40];
    strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%S%z", &lt);
    ctx_add(arr, "current_local_time", iso);   // clearer key so the agent prefers it over searching
}

// --- AXP2101 PMIC @ 0x34 on the shared BSP I2C bus (battery + PWR key) ------------------------
// Register-level reads mirroring XPowersLib (so no Arduino-C++ lib is vendored). We only do
// MEASUREMENT-enable + PWRKEY-IRQ-enable writes (battery ADC on, TS-pin off; latch PWRKEY presses
// so we can poll them) — exactly the read path the proven 01_AXP2101 example uses — and never
// touch power rails or charger settings.
#define AXP2101_ADDR        0x34
#define AXP_REG_STATUS1     0x00   // bit5 = VBUS good (USB power present); bit3 = battery connected
#define AXP_ST1_VBUS_GOOD   (1u << 5) // USB/VBUS present & valid → device is "plugged in"
#define AXP_ST1_BAT_PRESENT (1u << 3) // battery connected
#define AXP_REG_STATUS2     0x01   // (val >> 5): 0=standby/full, 1=charging, 2=discharging
#define AXP_REG_ADC_CTRL    0x30   // bit0 = batt-voltage ADC enable; bit1 = TS-pin measure
#define AXP_REG_IRQ_EN2     0x41   // INTEN2: enable PWRKEY short=bit3 / long=bit2 IRQ latching
#define AXP_REG_IRQ_STS2    0x49   // INTSTS2: PWRKEY short=bit3 / long=bit2 status (write-1-to-clear)
#define AXP_REG_BAT_PERCENT 0xA4   // 0..100
#define AXP_IRQ_PKEY_SHORT  (1u << 3)
#define AXP_IRQ_PKEY_LONG   (1u << 2)

static i2c_master_dev_handle_t s_axp;   // lazily added to the BSP bus

static esp_err_t axp_rd(uint8_t reg, uint8_t *val)
{ return i2c_master_transmit_receive(s_axp, &reg, 1, val, 1, 100); }
static esp_err_t axp_wr(uint8_t reg, uint8_t val)
{ uint8_t b[2] = { reg, val }; return i2c_master_transmit(s_axp, b, 2, 100); }

// Bring the AXP2101 up on the shared BSP I2C bus + enable battery measurement. Lazy & idempotent
// (bsp_i2c_init() is a no-op if already initialized), so call order vs the display doesn't matter.
static bool axp_ensure(void)
{
    if (s_axp) return true;
    if (bsp_i2c_init() != ESP_OK) return false;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return false;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                .device_address = AXP2101_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &cfg, &s_axp) != ESP_OK) { s_axp = NULL; return false; }
    uint8_t adc;   // measurement-only: enable batt ADC (needed for the % gauge), disable TS pin
    if (axp_rd(AXP_REG_ADC_CTRL, &adc) == ESP_OK) {
        adc = (uint8_t)((adc | (1u << 0)) & ~(1u << 1));
        axp_wr(AXP_REG_ADC_CTRL, adc);
    }
    uint8_t irqen; // latch PWRKEY short/long-press IRQs so device_power_key_short_press() can poll them
    if (axp_rd(AXP_REG_IRQ_EN2, &irqen) == ESP_OK)
        axp_wr(AXP_REG_IRQ_EN2, (uint8_t)(irqen | AXP_IRQ_PKEY_SHORT | AXP_IRQ_PKEY_LONG));
    return true;
}

// battery → {"percent":0-100,"plugged_in":<bool>,"status":"<words>"} (JSON-stringified). Skipped if
// no battery is present. `status` is an explicit, human-readable charge state so the model never has
// to infer plugged-vs-unplugged from a bare "charging:false" — which is true BOTH when running on
// battery AND when plugged in but full. That ambiguity was confusing the agent.
static void add_battery(cJSON *arr)
{
    if (!axp_ensure()) return;
    uint8_t s1, s2, pct;
    if (axp_rd(AXP_REG_STATUS1, &s1) != ESP_OK || !(s1 & AXP_ST1_BAT_PRESENT)) return;  // no battery
    if (axp_rd(AXP_REG_BAT_PERCENT, &pct) != ESP_OK || pct > 100) return;               // invalid gauge read
    bool plugged  = (s1 & AXP_ST1_VBUS_GOOD) != 0;                                       // USB power present
    bool charging = (axp_rd(AXP_REG_STATUS2, &s2) == ESP_OK) && ((s2 >> 5) == 0x01);
    const char *status = !plugged ? "on battery"                  // unplugged → discharging
                       : charging ? "charging"                    // plugged in, taking charge
                                  : "plugged in, not charging";   // plugged in but full / charge complete
    char val[80];
    snprintf(val, sizeof val, "{\"percent\":%u,\"plugged_in\":%s,\"status\":\"%s\"}",
             pct, plugged ? "true" : "false", status);
    ctx_add(arr, "battery", val);
}

// PWR button (AXP2101 PWRKEY) short-press since the last call. The PMIC latches short/long-press
// events in INTSTS2 (we poll it — the AXP IRQ pin isn't wired to a GPIO on this board). A long press
// is a hardware power-off the PMIC does on its own, so only short presses are reported (used by the
// chat_ui screen-power saver to wake the display). Both bits are cleared (write-1-to-clear).
bool device_power_key_short_press(void)
{
    if (!axp_ensure()) return false;
    uint8_t sts;
    if (axp_rd(AXP_REG_IRQ_STS2, &sts) != ESP_OK) return false;
    uint8_t hit = sts & (AXP_IRQ_PKEY_SHORT | AXP_IRQ_PKEY_LONG);
    if (hit) axp_wr(AXP_REG_IRQ_STS2, hit);          // clear only the PWRKEY bits we consumed
    return (sts & AXP_IRQ_PKEY_SHORT) != 0;
}

// True if running on battery (no USB/VBUS power) — gates idle WiFi-off so a plugged-in device stays
// connected + instant. Conservative: if the PMIC can't be read, report NOT on battery (don't shed WiFi).
bool device_tools_on_battery(void)
{
    if (!axp_ensure()) return false;
    uint8_t s1;
    if (axp_rd(AXP_REG_STATUS1, &s1) != ESP_OK) return false;
    return !(s1 & AXP_ST1_VBUS_GOOD);                // VBUS absent → on battery
}

// tts_voice → the configured Soniox TTS voice name (the assistant's own spoken voice), so the agent
// knows how it sounds and can refer to it. Always present (defaults to Adrian, matching the TTS client).
static void add_voice(cJSON *arr)
{
    char voice[APP_CFG_VAL_MAX];
    if (!app_cfg_get(APP_CFG_TTS_VOICE, voice, sizeof voice)) strlcpy(voice, "Adrian", sizeof voice);
    ctx_add(arr, "tts_voice", voice);
}

cJSON *device_context_build(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    add_local_time(arr);
    add_battery(arr);
    add_voice(arr);
    // Next P5 increment appends here: device_motion (QMI8658).

    if (cJSON_GetArraySize(arr) == 0) {   // nothing to report yet → send no context
        cJSON_Delete(arr);
        return NULL;
    }
    return arr;
}

// --- client tool registry (P7) --------------------------------------------------------------
// A tiny static registry of builtin client tools. Each entry holds the tool's AG-UI definition
// ({description, parameters JSON-Schema}, used to build RunAgentInput.tools) and its handler.
// Definitions persist for the process lifetime (built once at init).
#define MAX_TOOLS 6

typedef struct {
    const char     *name;
    cJSON          *def;    // {description, parameters}
    device_tool_fn  fn;
} tool_entry_t;

static tool_entry_t s_tools[MAX_TOOLS];
static int          s_tool_count;

void device_tools_register(const char *name, const cJSON *schema, device_tool_fn fn)
{
    if (!name || !fn || s_tool_count >= MAX_TOOLS) {
        ESP_LOGW(TAG, "register(%s) dropped (registry full or invalid)", name ? name : "(null)");
        return;
    }
    s_tools[s_tool_count++] = (tool_entry_t){ name, (cJSON *)schema, fn };
    ESP_LOGI(TAG, "tool registered: %s", name);
}

esp_err_t device_tools_dispatch(const char *name, const cJSON *args, cJSON **result)
{
    if (result) *result = NULL;
    for (int i = 0; i < s_tool_count; i++)
        if (name && strcmp(s_tools[i].name, name) == 0)
            return s_tools[i].fn(args, result);
    return ESP_ERR_NOT_FOUND;   // not one of our client tools — caller leaves it for the agent
}

// True if `name` is one of our registered client tools (so the device owes a TOOL result for it).
// Server/agent tools return false — the caller must not capture or try to answer those.
bool device_tools_is_client(const char *name)
{
    for (int i = 0; i < s_tool_count; i++)
        if (name && strcmp(s_tools[i].name, name) == 0) return true;
    return false;
}

// RunAgentInput.tools = [{name, description, parameters}] built from the registry (NULL if empty).
cJSON *device_tools_manifest(void)
{
    if (s_tool_count == 0) return NULL;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    for (int i = 0; i < s_tool_count; i++) {
        cJSON *t = cJSON_CreateObject();
        if (!t) continue;
        cJSON_AddStringToObject(t, "name", s_tools[i].name);
        const cJSON *desc   = cJSON_GetObjectItemCaseSensitive(s_tools[i].def, "description");
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(s_tools[i].def, "parameters");
        if (cJSON_IsString(desc)) cJSON_AddStringToObject(t, "description", desc->valuestring);
        if (params)               cJSON_AddItemToObject(t, "parameters", cJSON_Duplicate(params, true));
        cJSON_AddItemToArray(arr, t);
    }
    return arr;
}

// --- builtin: set_timer ---------------------------------------------------------------------
// One-shot countdown. The returned string is the agent-visible tool result; the live countdown and
// the fire alert are surfaced by the UI/main, which POLL the two accessors below — this component
// never calls into chat_ui (chat_ui already depends on device_tools, not the reverse).
static esp_timer_handle_t s_timer_h;
static volatile int64_t   s_timer_deadline_us;        // 0 = no active timer
static volatile bool      s_timer_fired;
static char               s_timer_label[40];

static void timer_fire_cb(void *arg)
{
    (void)arg;
    s_timer_deadline_us = 0;
    s_timer_fired = true;
    if (s_timer_q) { uint8_t sig = 1; xQueueSend(s_timer_q, &sig, 0); }   // wake the alert task (cb runs in task ctx)
    ESP_LOGI(TAG, "timer fired: %s", s_timer_label[0] ? s_timer_label : "(timer)");
}

QueueHandle_t device_tools_timer_queue(void) { return s_timer_q; }

static esp_err_t tool_set_timer(const cJSON *args, cJSON **result)
{
    const cJSON *secs  = cJSON_GetObjectItemCaseSensitive(args, "seconds");
    const cJSON *label = cJSON_GetObjectItemCaseSensitive(args, "label");
    int seconds = cJSON_IsNumber(secs) ? (int)secs->valuedouble : 0;
    if (seconds <= 0 || seconds > 86400) {            // 1 s .. 24 h
        if (result) *result = cJSON_CreateString("error: 'seconds' must be an integer 1..86400");
        return ESP_OK;                                // a (negative) tool RESULT, not a dispatch failure
    }
    strlcpy(s_timer_label, (cJSON_IsString(label) && label->valuestring) ? label->valuestring : "",
            sizeof s_timer_label);
    if (!s_timer_h) {
        const esp_timer_create_args_t ta = { .callback = timer_fire_cb, .name = "devtimer" };
        if (esp_timer_create(&ta, &s_timer_h) != ESP_OK) {
            if (result) *result = cJSON_CreateString("error: could not create timer");
            return ESP_OK;
        }
    }
    esp_timer_stop(s_timer_h);                        // replace any timer already running
    s_timer_fired = false;
    s_timer_deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    esp_timer_start_once(s_timer_h, (uint64_t)seconds * 1000000);
    char msg[80];
    snprintf(msg, sizeof msg, "Timer set for %d second%s%s%s", seconds, seconds == 1 ? "" : "s",
             s_timer_label[0] ? ": " : "", s_timer_label);
    if (result) *result = cJSON_CreateString(msg);
    return ESP_OK;
}

int device_tools_timer_remaining(void)
{
    int64_t dl = s_timer_deadline_us;
    if (dl == 0) return 0;
    int64_t left = dl - esp_timer_get_time();
    return left <= 0 ? 0 : (int)((left + 999999) / 1000000);
}

bool device_tools_timer_take_fired(char *label, size_t n)
{
    if (!s_timer_fired) return false;
    s_timer_fired = false;
    if (label && n) strlcpy(label, s_timer_label, n);
    return true;
}

// Build a tool def {description, parameters} from a description + a JSON-Schema string.
static cJSON *tool_def(const char *description, const char *params_schema_json)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddStringToObject(d, "description", description);
    cJSON *p = cJSON_Parse(params_schema_json);
    if (p) cJSON_AddItemToObject(d, "parameters", p);
    return d;
}

static void register_builtins(void)
{
    if (s_tool_count) return;   // idempotent
    device_tools_register(
        "set_timer",
        tool_def("Start a countdown timer; the device alerts the user when it elapses.",
                 "{\"type\":\"object\",\"properties\":{"
                 "\"seconds\":{\"type\":\"integer\",\"description\":\"Duration in seconds (1-86400)\"},"
                 "\"label\":{\"type\":\"string\",\"description\":\"Optional short name for the timer\"}},"
                 "\"required\":[\"seconds\"]}"),
        tool_set_timer);
    // set_alarm (PCF85063 RTC) and show_qr (lv_qrcode) register here next.
}
