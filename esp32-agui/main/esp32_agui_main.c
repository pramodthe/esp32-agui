// AG-UI on-device voice client — application entry point.
// P0/P0.5: WiFi (multi-SSID + captive portal). P1: streaming STT (Soniox). P2: AG-UI client.
// P3: chat UI on the AMOLED + BOOT-button push-to-talk. See docs/esp32-agui-plan.md.
//
// Turn model (P3 PTT): no continuous STT session. Hold the BOOT button → open the Soniox session
// and stream the mic (live transcript in the status line); release → stop the session and send the
// utterance to the AG-UI agent. Session is open only while held → no idle Soniox timeout, no
// streamed-silence garbage, and the button defines the turn boundary.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "iot_button.h"
#include "button_gpio.h"

#include "net_prov.h"
#include "app_cfg.h"
#include "speech_stt.h"
#include "speech_tts.h"
#include "speech_cfg.h"
#include "agui_client.h"
#include "device_tools.h"
#include "chat_ui.h"

#include "esp_codec_dev.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"

static const char *TAG = "esp32_agui";

#define PTT_GPIO    0                 // BOOT button: strapping pin at RESET only; normal input at runtime
#define IDLE_HINT   "Hold Top Button to talk"

static QueueHandle_t s_ptt_q;          // button events: 1 = press (down), 0 = release (up)
static volatile bool s_listening;      // a hold is in progress
static char s_ptt_final[512];          // finalized text accumulated across mid-hold endpoints
static char s_ptt_run[640];            // last interim ("running") transcript

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// Apply the configured POSIX TZ (from the portal dropdown; default UTC0) so localtime_r() yields
// local time for the local_time ambient-context entry. Re-call after a reconfigure that may change it.
static void apply_timezone(void)
{
    char tz[64];
    setenv("TZ", (app_cfg_get(APP_CFG_TZ, tz, sizeof tz) && tz[0]) ? tz : "UTC0", 1);
    tzset();
}

// --- Soniox transcript callbacks (fire from the ws task; only meaningful while holding PTT) ---
static void on_partial(const char *text, void *ctx)
{
    if (!s_listening || !text) return;
    strlcpy(s_ptt_run, text, sizeof s_ptt_run);
    char live[1024];
    snprintf(live, sizeof live, "%s%s", s_ptt_final, text);   // finalized so far + current interim
    chat_ui_status(live[0] ? live : "Listening...");
    ESP_LOGI("stt", "~ %s", live);
}
static void on_turn(const char *text, void *ctx)             // Soniox endpoint mid-hold: keep the segment
{
    if (!s_listening || !text || !*text) return;
    strlcat(s_ptt_final, text, sizeof s_ptt_final);
    strlcat(s_ptt_final, " ", sizeof s_ptt_final);
    s_ptt_run[0] = '\0';
}

// --- AG-UI run handlers (fire from the ptt task during agui_run) ---
// They drive the chat bubbles (TEXT_*) and the ephemeral status line (RUN_*/REASONING_*/
// TOOL_CALL_*) — P4 live agent activity: status shows what the agent is doing and clears
// when the reply text starts streaming.
static bool s_assist_started;          // assistant bubble open for the current message?
static bool s_run_error;               // run ended in error → keep the "Error" status visible

#define TTS_TEXT_MAX 2048              // cap the buffered reply (well under Soniox's 5000-char frame)
static char   s_tts_text[TTS_TEXT_MAX]; // whole reply accumulated across the turn → batch fallback
static size_t s_tts_len;
static bool   s_tts_streaming;         // P-b: this turn opened a live TTS stream (feeds as deltas arrive)

// P-c PTT barge-in: a press DURING a reply aborts it. s_responding is true for the whole
// run_agent_turn (the gate ptt_down_cb checks to decide "this press is a barge-in"); s_aborting is the
// one-shot the button cb sets so h_error suppresses the cancel-as-error and run_agent_turn bails clean.
static volatile bool s_responding;
static volatile bool s_aborting;

// --- P7 client tools: pending list ------------------------------------------------------------
// Client-tool calls the agent makes during a run are RECORDED here by h_tool_call (which runs INSIDE
// agui_run under s_lock — so it may only copy strings, never dispatch/re-run). run_agent_turn drains
// the list AFTER agui_run returns, executes each tool, returns a result, then re-runs. Owned solely
// by ptt_task (single producer/consumer, never concurrent) → no locking.
#define AGUI_TOOL_MAX_ITERS 5      // hard cap on run→tool→run cycles per utterance
#define PEND_MAX            4      // max client tool calls captured per run
#define TOOL_ID_MAX        64
#define TOOL_NAME_MAX      48
#define TOOL_ARGS_MAX     512

typedef struct {
    char id[TOOL_ID_MAX];
    char name[TOOL_NAME_MAX];
    char args[TOOL_ARGS_MAX];      // raw JSON string from the SDK ("{}" if empty)
} pending_tool_t;

static pending_tool_t s_pending[PEND_MAX];
static int            s_pending_n;        // client tool calls recorded this run
static bool           s_pending_overflow;

static void h_run_started(void *c)
{
    ESP_LOGI("agui", "thinking...");
    chat_ui_status("Thinking...");
    printf("\nAI: "); fflush(stdout);
}
static void h_text_delta(const char *d, void *c)
{
    if (!s_assist_started) {                 // first delta of this message → open a bubble…
        chat_ui_begin_assistant();
        chat_ui_status("");                  // …and clear the reasoning/tool indicator
        s_assist_started = true;
    }
    chat_ui_append_assistant(d);
    if (d && *d) {
        // P-b: open a live TTS stream on the first speakable delta (this runs inside the AG-UI SSE read
        // on ptt_task, so open() briefly stalls the SSE — lossless). If it can't open (e.g. concurrent-
        // TLS OOM), s_tts_streaming stays false and we fall back to batch after the run.
        if (!s_tts_streaming) {
            if (speech_tts_open() == ESP_OK) s_tts_streaming = true;
        }
        if (s_tts_streaming) speech_tts_feed(d);
        // Always also buffer the whole reply: the batch fallback for a delta-less / open-failed turn.
        size_t dl = strlen(d);
        if (s_tts_len + dl < sizeof s_tts_text) {
            memcpy(s_tts_text + s_tts_len, d, dl);
            s_tts_len += dl;
            s_tts_text[s_tts_len] = '\0';
        }
    }
    printf("%s", d); fflush(stdout);
}
static void h_text_end(void *c)
{
    s_assist_started = false;                // message done → the next TEXT_MESSAGE gets its own bubble
    printf("\n"); fflush(stdout);
}
static void h_reasoning(const char *d, bool active, void *c)
{
    if (active) chat_ui_status("Reasoning...");   // ephemeral; cleared when the reply text starts
    if (d && *d) ESP_LOGI("agui", "reasoning: %s", d);
}
static void h_tool_call(const char *id, const char *name, const char *args, void *c)
{
    char s[96];
    snprintf(s, sizeof s, "Using %s...", (name && *name) ? name : "tool");
    chat_ui_status(s);                       // transient chip; the next event overwrites it
    ESP_LOGI("agui", "TOOL_CALL %s (%s) args=%s", name, id, args);

    // Record ONLY tools we can execute. Server/agent tools are run by the agent itself; we owe no
    // result for them, so we must not capture (and later try to answer) them. Runs inside agui_run
    // under s_lock → copy three strings and return; NO dispatch, NO re-run (would deadlock).
    if (!device_tools_is_client(name)) return;
    if (s_pending_n >= PEND_MAX) { s_pending_overflow = true; return; }
    pending_tool_t *p = &s_pending[s_pending_n++];
    strlcpy(p->id,   id   ? id   : "",                sizeof p->id);
    strlcpy(p->name, name ? name : "",                sizeof p->name);
    strlcpy(p->args, (args && args[0]) ? args : "{}", sizeof p->args);
}
static void h_run_finished(void *c) { ESP_LOGI("agui", "run finished"); }
static void h_error(const char *m, void *c)
{
    if (s_aborting) {                        // deliberate barge-in cancel — not a failure
        ESP_LOGI("agui", "run cancelled (barge-in): %s", m);
        return;                              // leave s_run_error false + the UI to the new turn
    }
    ESP_LOGE("agui", "run error: %s", m);
    chat_ui_status("Error");
    s_run_error = true;
}

static const agui_handlers_t s_handlers = {
    .on_run_started  = h_run_started,
    .on_text_delta   = h_text_delta,
    .on_text_end     = h_text_end,
    .on_reasoning    = h_reasoning,
    .on_tool_call    = h_tool_call,
    .on_run_finished = h_run_finished,
    .on_error        = h_error,
};

static void tts_speak_reply(const char *text);   // defined with the speaker helpers below

// Render the user turn, run one AG-UI turn (streams the reply into the chat), restore the idle hint.
static void run_agent_turn(const char *text)
{
    char url[APP_CFG_VAL_MAX], token[APP_CFG_VAL_MAX];
    ESP_LOGI(TAG, "You: %s", text);
    chat_ui_add_user(text);

    if (!app_cfg_get(APP_CFG_AGUI_URL, url, sizeof url)) {
        ESP_LOGW(TAG, "no AG-UI URL configured");
        chat_ui_status("No AG-UI URL");
        return;
    }
    bool have_tok = app_cfg_get(APP_CFG_AGUI_TOKEN, token, sizeof token);

    s_responding     = true;                       // P-c: a BOOT press from here on is a barge-in
    s_assist_started = false;
    s_run_error      = false;
    s_tts_len        = 0;
    s_tts_text[0]    = '\0';
    s_tts_streaming  = false;                       // P-b: set true once h_text_delta opens a live stream
    chat_ui_status("Thinking...");
    agui_cfg_t acfg = { .endpoint = url, .auth_bearer = have_tok ? token : NULL, .thread_id = NULL };
    cJSON *tools    = device_tools_manifest();     // advertised on EVERY run (NULL ⇒ no tools)

    // The agent may call device client tools (set_timer, ...). Advertise them each run; after each
    // run, drain any recorded calls — execute on-device, append a tool result to history, then re-run
    // with NULL user_text so the agent continues. Bounded by AGUI_TOOL_MAX_ITERS. Every agui_run /
    // agui_tool_result call happens HERE on ptt_task with s_lock free between them (no deadlock).
    const char *user_text = text;                  // first run carries the utterance; continuations don't
    for (int iter = 0; iter < AGUI_TOOL_MAX_ITERS; iter++) {
        if (s_aborting) break;                     // P-c: barge-in landed between runs (e.g. tool exec) → stop
        s_pending_n        = 0;                    // reset capture for THIS run
        s_pending_overflow = false;

        cJSON *device_ctx = device_context_build();          // fresh ambient context each run
        agui_run(&acfg, user_text, device_ctx, tools, NULL, &s_handlers, NULL);  // BLOCKS; fills s_pending
        cJSON_Delete(device_ctx);

        if (s_aborting) {                          // P-c: barged in mid-run → drop the partial reply, bail
            agui_drop_partial_assistant();         // so a half-streamed assistant msg can't poison the next run
            break;
        }
        if (s_run_error)   break;                  // transport/RUN_ERROR → stop (h_error kept "Error" up)
        if (s_pending_n == 0) break;               // no client tool calls → turn complete
        if (s_pending_overflow) ESP_LOGW(TAG, "tool calls truncated to %d this run", PEND_MAX);

        // Execute each recorded client tool and round-trip a result into history.
        for (int i = 0; i < s_pending_n; i++) {
            pending_tool_t *p = &s_pending[i];
            cJSON *args   = cJSON_Parse(p->args[0] ? p->args : "{}");
            cJSON *result = NULL;
            esp_err_t err = device_tools_dispatch(p->name, args, &result);
            cJSON_Delete(args);
            if (err != ESP_OK || !result) {        // synthesize an error result so the agent still gets
                if (result) cJSON_Delete(result);  // a tool message (history stays paired) and can react
                result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", false);
                cJSON_AddStringToObject(result, "error", esp_err_to_name(err));
            }
            agui_tool_result(p->id, result);       // appends one Tool-role msg to s_agent history
            cJSON_Delete(result);
        }
        user_text = NULL;                          // continuation runs add NO user message
    }

    cJSON_Delete(tools);                           // cJSON_Delete(NULL) is safe
    if (s_aborting || s_run_error) {               // barge-in or real error → tear down any live stream
        if (s_tts_streaming) { speech_tts_cancel(); speech_tts_wait_drained(3000); }  // BIT_CANCEL → quick close + unlock
        s_responding = false;
        return;                                    // abort: new turn owns the UI; error: keep "Error" up
    }
    if (s_tts_streaming) {                          // P-b: the reply streamed live → finalize it
        chat_ui_status("Speaking...");
        speech_tts_finish();                        // text_end:true ONCE for the whole turn
        speech_tts_wait_drained(30000);             // play out the tail (30 s no-audio stall cap, not total)
        if (s_aborting) { s_responding = false; return; }  // barged in during the spoken tail
    } else if (s_tts_len > 0) {                     // delta-less / streaming-open-failed → P-a batch fallback
        chat_ui_status("Speaking...");
        tts_speak_reply(s_tts_text);
        if (s_aborting) { s_responding = false; return; }
    }
    chat_ui_status(IDLE_HINT);                     // back to the idle prompt
    s_responding = false;
}

// ---- PTT "go ahead and talk" beep -------------------------------------------------------------
// A short, subtle sine cue played on the ES8311 speaker the instant a hold starts. It plays from
// ptt_task BEFORE speech_stt_session_start() creates capture_task, so it never glitches an in-flight
// mic read. The speaker is a second esp_codec_dev OUT handle that coexists with the always-open mic
// IN handle on the single ES8311; both MUST use the same 16k/16/1 format (the shared codec/I2S
// clock's last set_fs wins — and it is NOT auto-enforced, so BEEP_SR is hard-pinned). Opening the
// speaker soft-resets the shared chip (clobbers the mic ADC gain) and its acoustic crosstalk lands
// in the live RX DMA ring — capture_task compensates by re-asserting the mic gain and draining the
// ring before it forwards any audio (see soniox_client capture_task).
#define BEEP_SR        16000   // MUST equal soniox_client DEFAULT_SR (the mic rate)
#define BEEP_MS        90      // duration of one beep
#define BEEP_FADE_MS   6       // linear fade in AND out — kills edge clicks
#define BEEP_VOL       75      // esp_codec_dev out-vol INDEX 0-100 (NOT dB; 0 would be -96 dB = silent).
                               // Shared by both tones at a known-good analog gain; loudness is set by the
                               // digital amplitude below, so neither tone overdrives the PA.
// Two tones share the speaker: a SUBTLE PTT "go ahead" cue and a LOUDER, higher-pitched timer alarm.
// Loudness ≈ amplitude / 32767 of full scale (clean headroom — alarm can go toward ~30000 for more).
#define CUE_FREQ       950     // Hz — subtle mid tone for the PTT cue
#define CUE_AMPL       6000    // ~0.18 FS — subtle (unchanged)
#define ALARM_FREQ     1760    // Hz — higher pitched, more attention-getting (loudness ramps in
                               // run_timer_alarm via ALARM_LEVELS, so there's no fixed alarm amplitude)
#define BEEP_SAMPLES   (BEEP_SR * BEEP_MS / 1000)        // 1440
#define BEEP_FADE_N    (BEEP_SR * BEEP_FADE_MS / 1000)   // 96

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static esp_codec_dev_handle_t s_spk;              // speaker OUT handle, opened once and kept open
static int16_t s_cue_pcm[BEEP_SAMPLES];           // PTT cue (subtle)
static int16_t s_alarm_pcm[BEEP_SAMPLES];         // timer alarm (loud, higher)
static bool    s_beep_ready;

// Fill `buf` with a `freq` sine at 16k mono, linear fade-in/out on the edges so the buffer's
// start/end discontinuities don't click. Peak ±ampl; keep ampl < 32767 so it can't digitally clip.
static void fill_tone(int16_t *buf, int freq, int ampl)
{
    const double w = 2.0 * M_PI * (double)freq / (double)BEEP_SR;
    for (int i = 0; i < BEEP_SAMPLES; i++) {
        double env = 1.0;                                            // anti-click amplitude envelope
        if (i < BEEP_FADE_N)                       env = (double)i / BEEP_FADE_N;
        else if (i >= BEEP_SAMPLES - BEEP_FADE_N)  env = (double)(BEEP_SAMPLES - 1 - i) / BEEP_FADE_N;
        buf[i] = (int16_t)lrint(sin(w * i) * env * ampl);
    }
}

static int s_spk_vol = -1;  // last out-vol set on s_spk (-1 = unknown / just (re)opened)
static int s_tts_vol = 90;  // spoken-reply volume 0-100, set by the volume buttons, loaded from NVS at boot

// Lazily bring up the speaker (once). Returns true if s_spk is open at 16k/16/mono. The speaker is a
// second esp_codec_dev OUT handle that coexists with the always-open mic on the single ES8311 — both
// MUST use the same 16k rate. Opened on the first beep/TTS write and kept open (no idle codec-close now).
static bool spk_ensure(void)
{
    if (s_spk) return true;
    s_spk = bsp_audio_codec_speaker_init();                 // pa_pin = GPIO46, raised automatically
    if (!s_spk) { ESP_LOGW(TAG, "speaker init failed"); return false; }
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 1, .sample_rate = BEEP_SR };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_OK) {         // same 16k as the mic → full-duplex OK
        ESP_LOGW(TAG, "speaker open failed");
        s_spk = NULL;                                       // retry on the next call
        return false;
    }
    s_spk_vol = -1;                                         // force the next user to set its volume
    ESP_LOGI(TAG, "speaker ready (16k mono)");
    return true;
}

// Set the shared speaker volume only when it actually changes (the beep + TTS want different levels;
// avoids an I2C write per audio chunk). 0-100 index, NOT dB.
static void spk_set_vol(int v)
{
    if (s_spk && v != s_spk_vol) { esp_codec_dev_set_out_vol(s_spk, v); s_spk_vol = v; }
}

// Lazily bring up the speaker (once) and play one precomputed tone. Blocks (~90 ms) until it is
// clocked out of the I2S TX DMA. Call ONLY from a task (ptt_task / timer task), never a button cb.
static void play_beep(const int16_t *pcm)
{
    if (!s_beep_ready) {                            // build the cue once; run_timer_alarm (re)fills the
        fill_tone(s_cue_pcm, CUE_FREQ, CUE_AMPL);   // alarm buffer at its current escalation level
        s_beep_ready = true;
    }
    if (!spk_ensure()) return;
    spk_set_vol(BEEP_VOL);
    esp_codec_dev_write(s_spk, (int16_t *)pcm, BEEP_SAMPLES * sizeof(int16_t));   // API wants non-const
}

static void play_ptt_beep(void) { play_beep(s_cue_pcm); }   // PTT "go ahead" cue (existing call sites)

// TTS PCM sink: the soniox_tts_client drain task hands decoded pcm_s16le (16k/mono) here — for both
// streaming (P-b) and batch (P-a) replies. main owns the single speaker handle, so TTS shares it with
// the beep + the low-power codec-close (no 2nd handle), and sets the louder spoken-reply volume here.
static void tts_pcm_write(const void *pcm, size_t bytes)
{
    if (!spk_ensure()) return;
    spk_set_vol(s_tts_vol);                          // live: volume-button changes take effect next chunk
    esp_codec_dev_write(s_spk, (void *)pcm, bytes);
}

// Batch fallback used when streaming didn't open (delta-less / open-failed run). Volume is handled by
// the sink (tts_pcm_write), so this is just the blocking speak.
static void tts_speak_reply(const char *text) { speech_tts_speak(text); }

// --- Volume control -----------------------------------------------------------------------------
// BOOT single-click = volume up, PWR short-press = volume down. The button/PWR callbacks just bump
// s_tts_vol (a plain int) so it takes effect LIVE — the TTS drain task reads it every chunk via
// tts_pcm_write, so you can turn a too-loud reply down mid-sentence. They also enqueue ev=3/4 to
// ptt_task for the non-time-critical feedback (a tick at the new level + status + NVS persist).
// Persisted so the level survives a power-cycle (the user powers off to save battery).
#define VOL_STEP    10
#define VOL_DEFAULT 90

static void vol_bump(int delta)             // runs in a button/screen cb — int write only, non-blocking
{
    int v = s_tts_vol + delta;
    if (v < 0) v = 0; else if (v > 100) v = 100;
    s_tts_vol = v;
    chat_ui_note_activity();                // wake the display on a volume press
    int e = delta > 0 ? 3 : 4;
    xQueueSend(s_ptt_q, &e, 0);             // ptt_task: tick + status + persist
}

// Play the short cue tone AT the current volume so the user hears the level (eyes-free). Task-context
// only (shares s_spk); ptt_task calls it, never concurrently with a TTS reply (it's blocked then).
static void play_vol_tick(void)
{
    if (!s_beep_ready) { fill_tone(s_cue_pcm, CUE_FREQ, CUE_AMPL); s_beep_ready = true; }
    if (!spk_ensure()) return;
    spk_set_vol(s_tts_vol);
    esp_codec_dev_write(s_spk, s_cue_pcm, BEEP_SAMPLES * sizeof(int16_t));
}

static void vol_feedback(void)              // ptt_task ev=3/4 handler
{
    char s[24];
    snprintf(s, sizeof s, "Volume: %d%%", s_tts_vol);
    chat_ui_status(s);
    play_vol_tick();
    char val[8];
    snprintf(val, sizeof val, "%d", s_tts_vol);
    app_cfg_set(APP_CFG_TTS_VOL, val);      // persist (NVS); a few writes/session — negligible wear
}

// --- Idle low-power (RESTORED to the known-good d0dd24b9 config) --------------------------------
// Removing this whole block broke the STT upload (transport_poll_write(0) / block-ack teardown) even on
// good WiFi — the WiFi/TLS path depends on PM being configured with light_sleep_enable=true + the
// NO_LIGHT_SLEEP lock HELD while active (released only on battery-idle). On battery-idle (display off)
// lp_idle sheds WiFi + the codec and releases the lock so the CPU light-sleeps; lp_wake reverses it.
// Plugged-in stays fully on (lp_idle is a no-op on USB), so no latency cost there.
static volatile bool        s_lp_suspended;
static SemaphoreHandle_t    s_lp_mutex;
static esp_pm_lock_handle_t s_lp_lock;     // NO_LIGHT_SLEEP — HELD while active, released only when idle
static esp_pm_lock_handle_t s_cpu_lock;    // CPU_FREQ_MAX  — HELD only for the duration of a turn (latency)

static void lp_wake(void)   // bring WiFi + codec back if shed; exactly-once
{
    if (!s_lp_mutex) return;
    xSemaphoreTake(s_lp_mutex, portMAX_DELAY);
    if (s_lp_suspended) {
        s_lp_suspended = false;
        if (s_lp_lock) esp_pm_lock_acquire(s_lp_lock);   // no light sleep while active
        speech_stt_mic_start();                       // re-enable the mic I2S (+ gain) for the turn
        net_wifi_resume();
    }
    xSemaphoreGive(s_lp_mutex);
}

static void lp_idle(void)   // shed everything when idle — only on battery (plugged-in stays connected)
{
    // Never shed mid-response (a long spoken reply can outlast the 60 s screen-idle blank).
    if (!s_lp_mutex || s_lp_suspended || s_responding || !device_tools_on_battery()) return;
    xSemaphoreTake(s_lp_mutex, portMAX_DELAY);
    if (!s_lp_suspended) {
        s_lp_suspended = true;
        speech_stt_mic_stop();                                // disable the mic I2S (RX)
        if (s_spk) { esp_codec_dev_close(s_spk); s_spk = NULL; s_spk_vol = -1; } // disable the speaker I2S (TX)
        net_wifi_suspend();
        if (s_lp_lock) esp_pm_lock_release(s_lp_lock);           // WiFi+I2S now off → allow light sleep
    }
    xSemaphoreGive(s_lp_mutex);
}

static void lp_set(bool display_on)   // chat_ui display-state hook (runs on the screen-power task)
{
    if (display_on) lp_wake();
    else            lp_idle();
}

static void power_mgmt_start(void)
{
    s_lp_mutex = xSemaphoreCreateMutex();
    esp_pm_config_t pmc = { .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = true };
    if (esp_pm_configure(&pmc) != ESP_OK) { ESP_LOGW(TAG, "PM configure failed (CONFIG_PM_ENABLE?)"); return; }
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "active", &s_lp_lock) != ESP_OK) return;
    esp_pm_lock_acquire(s_lp_lock);   // active at boot → no light sleep until lp_idle releases it
    // Latency: a CPU_FREQ_MAX lock acquired only for the span of a turn (turn_perf) so the 3 TLS
    // handshakes (STT/AG-UI/TTS) + JSON parsing run at 240 MHz instead of ramping up from the 80 MHz
    // DFS floor between network round-trips. NOT held at idle, so DFS still saves power between turns.
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "turn", &s_cpu_lock) != ESP_OK)
        ESP_LOGW(TAG, "PM: CPU_FREQ_MAX lock create failed (turns run at DFS speed)");
    ESP_LOGI(TAG, "PM: light sleep when idle (WiFi + codec off, on battery)");
}

// Full-throttle for the span of a turn: low-latency WiFi (PS=NONE) + CPU pinned to 240 MHz. Paired
// 1:1 (one true at PRESS, one false on every turn-exit) so the counted PM lock can't drift.
static void turn_perf(bool on)
{
    net_low_latency(on);
    if (s_cpu_lock) { if (on) esp_pm_lock_acquire(s_cpu_lock); else esp_pm_lock_release(s_cpu_lock); }
}

// PTT state machine: press → open STT + stream; release → stop, assemble the utterance, run it.
static void ptt_task(void *arg)
{
    for (;;) {
        int ev;
        if (xQueueReceive(s_ptt_q, &ev, portMAX_DELAY) != pdTRUE) continue;

        if (ev == 1 && !s_listening) {                 // PRESS (incl. a barge-in restart)
            s_aborting   = false;                      // P-c: consume the barge-in flags for the new turn
            s_responding = false;                      // (the aborted turn is fully torn down by now)
            s_ptt_final[0] = '\0';
            s_ptt_run[0]   = '\0';
            s_listening = true;
            lp_wake();                                 // woke from battery-idle? bring WiFi + codec back…
            if (!net_is_connected()) {                 // …and wait for the link before streaming to Soniox
                chat_ui_status("Connecting...");
                for (int i = 0; i < 100 && !net_is_connected(); i++) vTaskDelay(pdMS_TO_TICKS(50)); // ~5s
            }
            if (!net_is_connected()) {                 // gave up → don't open a doomed session
                s_listening = false;
                chat_ui_status("No WiFi");
                ESP_LOGW(TAG, "wake: WiFi did not reconnect");
                continue;
            }
            turn_perf(true);                           // low-latency WiFi + 240 MHz CPU for the whole turn
            chat_ui_status("Listening...");
            play_ptt_beep();                           // "go ahead" cue; plays & returns before capture starts
            speech_stt_cfg_t scfg = { 0 };               // api_key from NVS via speech_cfg
            if (speech_stt_session_start(&scfg, on_partial, on_turn, NULL) != ESP_OK) {
                s_listening = false;
                turn_perf(false);                      // no turn will run; restore power-save now
                chat_ui_status("STT error");
                ESP_LOGE(TAG, "STT failed to start");
            }
        } else if (ev == 0 && s_listening) {           // RELEASE
            s_listening = false;
            speech_stt_session_stop();                     // ws task is gone after this; buffers are stable
            if (speech_stt_last_error()) {                 // STT upload/transport died (e.g. hotspot congestion)
                ESP_LOGW(TAG, "STT failed: %s", speech_stt_last_error());
                chat_ui_status("Network — hold to retry");   // don't silently drop the turn
                turn_perf(false);
                continue;
            }
            char turn[1024];
            snprintf(turn, sizeof turn, "%s%s", s_ptt_final, s_ptt_run);
            char *t = turn;                            // trim surrounding whitespace
            while (*t == ' ' || *t == '\t') t++;
            size_t n = strlen(t);
            while (n && (t[n-1] == ' ' || t[n-1] == '\t' || t[n-1] == '\n')) t[--n] = '\0';
            if (*t) run_agent_turn(t);             // owns its terminal status (idle hint / "Error")
            else    chat_ui_status(IDLE_HINT);
            turn_perf(false);                      // turn done (reply streamed) → back to power-save

        } else if (ev == 2 && !s_listening) {      // DOUBLE-TAP → reopen setup portal (change endpoint etc.)
            chat_ui_status("Setup: join 'AMOLED-setup'");
            ESP_LOGI(TAG, "reconfigure: opening portal (only entered fields are saved)");
            net_portal_start("AMOLED-setup");
            bool saved = net_portal_wait_saved(180000);   // up to 3 min, then auto-close
            net_portal_stop();
            ESP_LOGI(TAG, "reconfigure: %s", saved ? "saved" : "timed out");
            if (saved) {
                apply_timezone();          // P5: TZ may have changed in the portal
                char v[8];                 // screen blank timeout may have changed → apply live (no reboot)
                if (app_cfg_get(APP_CFG_SCREEN_TO, v, sizeof v)) {
                    int n = atoi(v);
                    if (n >= 0 && n <= 86400) chat_ui_set_screen_timeout_s(n);
                }
                char ia[4];                // idle-animation flag may have changed → apply live
                chat_ui_set_idle_anim_enabled(app_cfg_get(APP_CFG_IDLE_ANIM, ia, sizeof ia) && ia[0] == '1');
            }
            chat_ui_status(IDLE_HINT);

        } else if (ev == 3 || ev == 4) {           // VOLUME up/down feedback (s_tts_vol already bumped)
            vol_feedback();                        // tick at the new level + "Volume: N%" + persist
        }
    }
}

// Any BOOT-button interaction wakes the display (note_activity un-arms a PWR-forced-off screen too),
// so reaching for the button to talk lights the screen even if it had blanked.
static void ptt_down_cb(void *btn, void *ctx)
{
    chat_ui_note_activity();
    if (s_responding) {                  // P-c BARGE-IN: a press while a reply is in flight aborts it
        ESP_LOGI(TAG, "barge-in: aborting active reply");
        s_aborting = true;               // set BEFORE agui_abort so h_error (fires inline) suppresses "Error"
        agui_abort();                    // cancel the AG-UI run (no-op if already past it)
        speech_tts_cancel();             // stop TTS playback (no-op if not speaking)
    }
    int e = 1; xQueueSend(s_ptt_q, &e, 0);   // enqueue the press; ptt_task starts a fresh turn once unblocked
}
static void ptt_up_cb(void *btn, void *ctx)   { chat_ui_note_activity(); int e = 0; xQueueSend(s_ptt_q, &e, 0); }  // release
static void ptt_dbl_cb(void *btn, void *ctx)  { chat_ui_note_activity(); int e = 2; xQueueSend(s_ptt_q, &e, 0); }  // double-tap → setup
static void ptt_vol_cb(void *btn, void *ctx)  { vol_bump(VOL_STEP); }    // BOOT single-click → volume UP

// Touch-to-talk (eyes-free): long-press anywhere on the screen → same as a BOOT hold. Fires from the
// LVGL task (display lock held) via chat_ui_set_talk_cb, so it only flips flags + enqueues (the same
// non-blocking contract as the button cbs). ev: 1 = hold start, 0 = release.
static void talk_cb(int ev, void *ctx)
{
    chat_ui_note_activity();
    if (ev == 1 && s_responding) {           // barge-in parity with the BOOT hold
        ESP_LOGI(TAG, "barge-in (touch): aborting active reply");
        s_aborting = true;
        agui_abort();
        speech_tts_cancel();
    }
    int e = ev; xQueueSend(s_ptt_q, &e, 0);
}

// PWR (AXP2101 PWRKEY) short press → volume DOWN. Fires from chat_ui's screen-power task.
static void pwr_vol_cb(void) { vol_bump(-VOL_STEP); }

static esp_err_t ptt_button_init(void)
{
    // BOOT: a quick TAP = volume up; a HOLD (>= long_press_time) = push-to-talk (fallback to the
    // touchscreen invoke); a DOUBLE-tap = setup portal. SINGLE_CLICK waits out the double-click window,
    // so a double-tap fires the portal only (no stray volume bump).
    button_config_t bcfg = { .long_press_time = 300 };
    button_gpio_config_t gcfg = { .gpio_num = PTT_GPIO, .active_level = 0 };   // BOOT pulls GPIO0 low
    button_handle_t btn;
    esp_err_t err = iot_button_new_gpio_device(&bcfg, &gcfg, &btn);
    if (err != ESP_OK) return err;
    iot_button_register_cb(btn, BUTTON_SINGLE_CLICK,     NULL, ptt_vol_cb,  NULL);  // tap → volume up
    iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, ptt_down_cb, NULL);  // hold → talk (fallback)
    iot_button_register_cb(btn, BUTTON_PRESS_UP,         NULL, ptt_up_cb,   NULL);  // release → stop + run
    iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK,     NULL, ptt_dbl_cb,  NULL);  // double-tap → setup portal
    return ESP_OK;
}

// A firing timer RINGS until the screen is tapped: bursts of 4 quick beeps with a big red ring
// flashing on/off in sync, then a dark pause, repeating — and getting LOUDER every few cycles. Runs
// on the timer task. chat_ui_alarm_set() shows the red-ring overlay + suspends the idle power-saver;
// we poll chat_ui_touch_idle_ms() for the dismiss tap (object-independent, so a tap anywhere counts).
// Auto-stops after ALARM_MAX_MS, or if BOOT is held to start a turn (s_listening).
#define ALARM_BRIGHT     90
#define ALARM_BEEPS      4
#define ALARM_GAP_MS     150
#define ALARM_PAUSE_MS   900
#define ALARM_MAX_MS     120000
#define ALARM_CYCLES_PER_LEVEL 4   // beep cycles at each loudness before stepping up
// Escalating loudness — int16 amplitudes (~/32767 of full scale). Start ~half the comfortable level
// and step up every ALARM_CYCLES_PER_LEVEL cycles until dismissed; top stays clear of clipping.
static const int ALARM_LEVELS[] = { 6500, 10000, 15000, 21000, 28000 };  // ~0.20,0.30,0.46,0.64,0.85 FS
#define ALARM_NLEVELS ((int)(sizeof ALARM_LEVELS / sizeof ALARM_LEVELS[0]))

// True once a fresh dismiss tap is seen — but only after the screen has first gone quiet, so a tap
// just before the timer fired doesn't instantly dismiss it. `*armed` persists across calls.
static bool alarm_dismiss_tap(bool *armed)
{
    uint32_t ti = chat_ui_touch_idle_ms();
    if (ti > 600) *armed = true;            // quiet (finger up / no recent touch) → ready to accept a tap
    return *armed && ti < 250;              // a touch within the last 250ms
}

static void run_timer_alarm(const char *label)
{
    chat_ui_note_activity();
    chat_ui_alarm_set(true);                 // black overlay + red ring; suspends the idle saver

    int64_t start = esp_timer_get_time();
    bool armed = false, done = false;
    int  cycle = 0, level = -1;
    while (!done && !s_listening && (esp_timer_get_time() - start) < (int64_t)ALARM_MAX_MS * 1000) {
        int want = cycle / ALARM_CYCLES_PER_LEVEL;            // escalate loudness over cycles
        if (want >= ALARM_NLEVELS) want = ALARM_NLEVELS - 1;
        if (want != level) { level = want; fill_tone(s_alarm_pcm, ALARM_FREQ, ALARM_LEVELS[level]); }

        for (int i = 0; i < ALARM_BEEPS && !done; i++) {
            chat_ui_alarm_flash(true);           // red ring ON (LVGL paints it; panel stays lit)…
            vTaskDelay(pdMS_TO_TICKS(20));        // let the "on" frame land before the beep
            play_beep(s_alarm_pcm);              // ~90ms (blocks)
            chat_ui_alarm_flash(false);          // …ring hidden (dark) between
            vTaskDelay(pdMS_TO_TICKS(ALARM_GAP_MS));
            if (alarm_dismiss_tap(&armed) || s_listening) done = true;
        }
        for (int t = 0; t < ALARM_PAUSE_MS && !done; t += 50) {   // dark pause, polling for the tap
            vTaskDelay(pdMS_TO_TICKS(50));
            if (alarm_dismiss_tap(&armed) || s_listening) done = true;
        }
        cycle++;
    }
    chat_ui_alarm_set(false);                // remove overlay; resume saver; screen on
    bsp_display_brightness_set(ALARM_BRIGHT);
    char msg[72];
    snprintf(msg, sizeof msg, "Timer done%s%s", label[0] ? ": " : "", label);
    chat_ui_status(msg);
}

// Poll the device timer (~1s): when a set_timer elapses, ring the alarm until the screen is tapped.
static void timer_alert_task(void *arg)
{
    QueueHandle_t q = device_tools_timer_queue();
    char label[40];
    for (;;) {
        uint8_t sig;
        xQueueReceive(q, &sig, portMAX_DELAY);   // BLOCKS until a set_timer fires — no periodic wake,
        if (device_tools_timer_take_fired(label, sizeof label))   // so the CPU can light-sleep until then
            run_timer_alarm(label);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AG-UI voice client booting (P0-P3)");
    init_nvs();

    device_tools_init();
    chat_ui_init();
    agui_client_init();

    // Provision until we have WiFi + Soniox key + AG-UI URL.
    ESP_ERROR_CHECK(net_prov_init());
    for (;;) {
        bool wifi_ok = net_is_connected() || (net_connect_saved(15000) == ESP_OK);
        bool key_ok  = speech_cfg_has_key();
        bool url_ok  = app_cfg_has(APP_CFG_AGUI_URL);
        if (wifi_ok && key_ok && url_ok) break;
        chat_ui_status("Setup: join 'AMOLED-setup'");
        ESP_LOGW(TAG, "provisioning (wifi=%d key=%d url=%d) — opening 'AMOLED-setup'", wifi_ok, key_ok, url_ok);
        net_portal_start("AMOLED-setup");
        net_portal_wait_saved(0);
        net_portal_stop();
    }
    net_start_auto_reconnect();
    apply_timezone();                  // P5: load POSIX TZ (default UTC0) for local_time
    net_sntp_start();                  // P5: sync wall-clock time for ambient context (local_time)
    ESP_LOGI(TAG, "network + keys ready");

    // Bring the mic up now; the Soniox WSS opens only on a PTT press.
    if (speech_stt_init() != ESP_OK) ESP_LOGE(TAG, "mic init failed");
    if (speech_tts_init(tts_pcm_write) != ESP_OK) ESP_LOGE(TAG, "tts init failed");  // P-a: spoken replies

    // Spoken-reply volume from NVS (set by the volume buttons); default VOL_DEFAULT if unset.
    { char v[8]; if (app_cfg_get(APP_CFG_TTS_VOL, v, sizeof v)) { int n = atoi(v); if (n >= 0 && n <= 100) s_tts_vol = n; } }

    power_mgmt_start();   // low-power: light sleep when idle (creates the lp mutex before ptt_task uses it)

    // Push-to-talk: long-press the SCREEN (eyes-free) or hold BOOT. BOOT tap = vol up, PWR = vol down.
    s_ptt_q = xQueueCreate(8, sizeof(int));
    // agui_run runs on this stack: TLS handshake + the C++ SDK (nlohmann JSON serialize/parse is
    // recursive + C++ exception unwinding), so it needs more headroom than the old cJSON path.
    xTaskCreate(ptt_task, "ptt", 16384, NULL, 5, NULL);
    xTaskCreate(timer_alert_task, "tmralert", 4096, NULL, 3, NULL);   // P7: surface set_timer firings
    if (ptt_button_init() != ESP_OK) ESP_LOGE(TAG, "PTT button init failed");
    chat_ui_status(IDLE_HINT);
    chat_ui_set_talk_cb(talk_cb, NULL);    // touch-to-talk: long-press the screen → start/stop a turn
    chat_ui_set_pwrkey_cb(pwr_vol_cb);     // PWR short-press → volume down
    chat_ui_set_power_cb(lp_set);          // low-power: shed WiFi+codec when the display blanks (on battery)
    // Screen blank timeout from NVS (captive portal): default 60 s, 0 = always on.
    { char v[8]; int s = 60; if (app_cfg_get(APP_CFG_SCREEN_TO, v, sizeof v)) { int n = atoi(v); if (n >= 0 && n <= 86400) s = n; }
      chat_ui_screen_power_start(s); }   // blank after s idle (wake on touch / PWR key / activity); 0 = always on
    { char v[4]; chat_ui_set_idle_anim_enabled(app_cfg_get(APP_CFG_IDLE_ANIM, v, sizeof v) && v[0] == '1'); }  // idle screensaver
    ESP_LOGI(TAG, "ready — long-press the screen or hold BOOT to talk");

    // Heartbeat: link + heap (internal RAM is the scarce one with the display) + UI liveness.
    char ip[16];
    uint32_t ui_last = chat_ui_ui_ticks();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        // UI liveness: the 1 Hz LVGL-task beacon must have moved between heartbeats. A stall means
        // the LVGL task is wedged holding the display mutex (historically: a brightness tx_param
        // racing a flush ate the trans-done) — everything else keeps running, so say it loudly.
        uint32_t ui_now = chat_ui_ui_ticks();
        bool ui_ok = (ui_now != ui_last);
        ui_last = ui_now;
        if (!ui_ok) ESP_LOGE(TAG, "UI STALLED: LVGL task hasn't run for a full heartbeat interval");
        if (net_get_ip_str(ip, sizeof ip)) {
            if (!net_time_synced()) net_time_http_fallback();   // P5: set clock via HTTPS if NTP is blocked
            // internal_max = largest contiguous internal block — TLS/lwIP send buffers need
            // contiguous internal RAM, so this matters more than total free when sessions fail.
            ESP_LOGI(TAG, "heartbeat: online ip=%s listening=%d free=%u internal=%u internal_max=%u psram=%u ui=%s",
                     ip, s_listening, (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     ui_ok ? "ok" : "STALLED");
        } else {
            ESP_LOGW(TAG, "heartbeat: offline");
        }
    }
}
