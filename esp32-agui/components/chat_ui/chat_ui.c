// chat_ui — LVGL 8.4 chat UI on the SH8601 AMOLED (368×448). See include/chat_ui.h.
//
// P3: display bring-up (BSP) + chat bubbles (user right / assistant left, streaming) + a
// status line. All public calls come from non-LVGL tasks (agent task, soniox ws task), so
// each wraps its LVGL work in bsp_display_lock()/unlock(). Interrupt prompt + QR are P6.

#include "chat_ui.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"
#include "device_tools.h"
#include "alarm_img.h"
#include "lvgl.h"

static const char *TAG = "chat_ui";

#define SAFE_INSET    22         // round-corner safe zone (panel ~50px radius)
#define STATUS_H      34
#define BUBBLE_MAXW   78         // % of chat width
#define CHAT_W        (BSP_LCD_H_RES - 2 * SAFE_INSET)
#define LBL_MAXW      (CHAT_W * BUBBLE_MAXW / 100 - 18)   // px wrap cap (bubble pad 9*2)
#define COL_BG        0x000000
#define COL_USER      0x2563EB   // blue
#define COL_ASSIST    0x2C2C2E   // dark grey
#define MAX_BUBBLES   30         // prune oldest rows beyond this
#define CHAT_FONT     (&lv_font_montserrat_20)   // bubbles + status line (one place to retune)
// Cap chars per assistant bubble so one long reply can't become a multi-thousand-px LONG_WRAP label
// (heavy to re-measure on every streamed delta and to redraw when scrolled). At ~234px wrap /
// Montserrat 20 (~23 ch/line) 700 chars ≈ ~30 lines ≈ ~720px tall: cheap to render, and overflow
// spills into a new bubble. (Coordinate overflow is handled separately by LV_USE_LARGE_COORD=y, so
// this cap is about render cost / throughput, not the coord ceiling.)
#define ASSIST_BUBBLE_MAX_CHARS  700

#define SCREEN_ON_BRIGHTNESS   90       // % brightness when awake
#define SCREEN_IDLE_TIMEOUT_MS 60000    // default: blank the AMOLED after this much inactivity
#define SCREEN_POLL_MS         200      // screen-power tick = wake latency + PWR-key poll cadence

static lv_obj_t *s_chat;         // scrollable flex column of message rows
static lv_obj_t *s_status;       // top status label
static lv_obj_t *s_assist_lbl;   // label of the in-progress assistant bubble (streaming)
static char      s_assist_buf[2048];   // accumulated assistant text (to re-measure on each delta)
static size_t    s_assist_len;

// Make text renderable by the (Latin-only) Montserrat font: transliterate common punctuation
// (em/en dash, curly quotes, ellipsis, nbsp) to ASCII, and DROP any other multi-byte codepoint
// (emoji, CJK, accents) the font has no glyph for — otherwise they render as tofu. v1 defers
// emoji/CJK fonts (they balloon flash); dropping beats boxing. LLM agents emit all of these.
static void sanitize(const char *src, char *dst, size_t dstsz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 4 < dstsz; ) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x80) { dst[o++] = (char)c; i++; continue; }            // ASCII
        if (c == 0xE2 && (unsigned char)src[i + 1] == 0x80) {           // U+20xx punctuation
            const char *rep = NULL;
            switch ((unsigned char)src[i + 2]) {
                case 0x93: case 0x94: rep = "-";   break;   // en / em dash
                case 0x98: case 0x99: rep = "'";   break;   // ‘ ’
                case 0x9C: case 0x9D: rep = "\"";  break;   // “ ”
                case 0xA6:            rep = "...";  break;   // ellipsis
            }
            if (rep) { while (*rep) dst[o++] = *rep++; i += 3; continue; }
        }
        if (c == 0xC2 && (unsigned char)src[i + 1] == 0xA0) { dst[o++] = ' '; i += 2; continue; } // nbsp
        // any other multi-byte codepoint (emoji/CJK/accent): drop the whole sequence (no glyph)
        int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        for (int k = 0; k < len && src[i]; k++) i++;
    }
    dst[o] = '\0';
}

// Set a label's text (sanitized) AND bound its width to the wrapped size, so LONG_WRAP actually
// wraps (a content-sized label ignores max_width and just clips) while short bubbles still shrink.
static void apply_wrapped(lv_obj_t *lbl, const char *text)
{
    char clean[2048];
    sanitize(text ? text : "", clean, sizeof clean);
    lv_point_t sz;
    lv_txt_get_size(&sz, clean[0] ? clean : " ", CHAT_FONT, 0, 0, LBL_MAXW, LV_TEXT_FLAG_NONE);
    lv_obj_set_width(lbl, sz.x);
    lv_label_set_text(lbl, clean);
}

static void scroll_bottom(void)
{
    uint32_t n = lv_obj_get_child_cnt(s_chat);
    if (!n) return;
    lv_obj_update_layout(s_chat);   // lay out the just-added/grown bubble BEFORE scrolling to it
    lv_obj_scroll_to_view(lv_obj_get_child(s_chat, n - 1), LV_ANIM_OFF);
}

// Add a bubble (user → right, assistant → left) and return its text label.
static lv_obj_t *add_bubble(bool user, uint32_t color, const char *text)
{
    while (lv_obj_get_child_cnt(s_chat) >= MAX_BUBBLES)
        lv_obj_del(lv_obj_get_child(s_chat, 0));            // prune oldest

    lv_obj_t *row = lv_obj_create(s_chat);                  // full-width transparent row
    lv_obj_remove_style_all(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);          // press falls through to s_chat (touch-to-talk)
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *bub = lv_obj_create(row);
    lv_obj_remove_style_all(bub);
    lv_obj_clear_flag(bub, LV_OBJ_FLAG_CLICKABLE);          // press falls through to s_chat (touch-to-talk)
    lv_obj_set_width(bub, LV_SIZE_CONTENT);     // bubble grows to its label…
    lv_obj_set_height(bub, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bub, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bub, 14, 0);
    lv_obj_set_style_pad_all(bub, 9, 0);

    lv_obj_t *lbl = lv_label_create(bub);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, CHAT_FONT, 0);
    apply_wrapped(lbl, text);       // bounds width → wraps; sanitizes punctuation
    return lbl;
}

// Screen-power state shared by the activity hook (below) and screen_power_task. Defined here so the
// LVGL event cb installed in chat_ui_init can stamp activity; the rest of the saver lives lower down.
static volatile uint32_t s_last_activity_ms;           // monotonic ms of last activity (touch/UI/PWR)
static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// LVGL event cb: any touch/scroll on the UI is "activity". Runs inside the LVGL task (lock already
// held by lv_timer_handler), so it just stamps the time — no lock, no I2C, no contention.
static void chat_ui_activity_evt_cb(lv_event_t *e)
{
    (void)e;
    s_last_activity_ms = now_ms();
}

static void chat_ui_talk_evt_cb(lv_event_t *e);   // touch-to-talk; defined with the screen-power code below

esp_err_t chat_ui_init(void)
{
    if (!bsp_display_start()) { ESP_LOGE(TAG, "bsp_display_start failed"); return ESP_FAIL; }
    if (!bsp_display_lock(3000)) { ESP_LOGE(TAG, "display lock timed out"); return ESP_FAIL; }
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Status line: a fixed-width clipping box holding a single-line label. Long live transcripts
    // scroll left so the tail (latest words) stays on screen; short messages center. (clips to the
    // box edges, respecting the round-corner safe zone — not the screen edge.)
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, CHAT_W, STATUS_H);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, SAFE_INSET);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    s_status = lv_label_create(box);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_CLIP);   // one line, full content width, no dots
    lv_obj_set_style_text_color(s_status, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_text_font(s_status, CHAT_FONT, 0);
    lv_obj_set_pos(s_status, 0, 7);
    lv_label_set_text(s_status, "Connecting...");   // not ready until WiFi is up + boot sets IDLE_HINT

    s_chat = lv_obj_create(scr);
    lv_obj_remove_style_all(s_chat);
    lv_obj_set_size(s_chat, BSP_LCD_H_RES - 2 * SAFE_INSET,
                    BSP_LCD_V_RES - 2 * SAFE_INSET - STATUS_H);
    lv_obj_align(s_chat, LV_ALIGN_TOP_MID, 0, SAFE_INSET + STATUS_H);
    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_chat, 8, 0);
    lv_obj_add_flag(s_chat, LV_OBJ_FLAG_SCROLLABLE);   // remove_style_all leaves flags, but be explicit
    lv_obj_set_scroll_dir(s_chat, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat, LV_SCROLLBAR_MODE_OFF);

    // Touch/scroll = activity for the screen-power saver. This event cb runs INSIDE the LVGL task
    // (already holding lvgl_mutex), so it bumps s_last_activity_ms without ever taking a lock — i.e.
    // it works even when screen_power_task can't grab the lock to read lv_disp_get_inactive_time().
    // Cover the chat list (scroll) and the screen (raw press/release) so reading/flinging keeps the
    // panel awake. LV_EVENT_PRESSING/SCROLL fire continuously, so the screen never blanks mid-gesture.
    lv_obj_add_event_cb(s_chat, chat_ui_activity_evt_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_chat, chat_ui_activity_evt_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr,    chat_ui_activity_evt_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr,    chat_ui_activity_evt_cb, LV_EVENT_RELEASED, NULL);

    // Touch-to-talk: long-press anywhere → start a turn, release → stop. Registered on BOTH the root
    // (presses over the blank/status area) AND the chat list (which covers most of the screen — a press
    // there goes to s_chat and does NOT bubble to scr). A drag on the chat still scrolls, because LVGL
    // suppresses LONG_PRESSED once a scroll begins; a still hold fires it.
    lv_obj_add_event_cb(scr,    chat_ui_talk_evt_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(scr,    chat_ui_talk_evt_cb, LV_EVENT_RELEASED,     NULL);
    lv_obj_add_event_cb(s_chat, chat_ui_talk_evt_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_chat, chat_ui_talk_evt_cb, LV_EVENT_RELEASED,     NULL);

    // Raise the touch scroll threshold so a STILL hold (with the few px of capacitive jitter) over the
    // scrollable chat isn't read as a scroll — which would cancel the long-press. A deliberate drag
    // (> this many px) still scrolls. Default is 10; 30 reliably distinguishes hold-to-talk from scroll.
    lv_indev_t *indev = lv_indev_get_next(NULL);
    if (indev && indev->driver) indev->driver->scroll_limit = 30;

    bsp_display_unlock();
    bsp_display_brightness_set(SCREEN_ON_BRIGHTNESS);
    ESP_LOGI(TAG, "chat UI up (%dx%d, LVGL 8.4)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}

void chat_ui_add_user(const char *text)
{
    chat_ui_note_activity();
    if (!s_chat || !bsp_display_lock(1000)) return;
    add_bubble(true, COL_USER, text);
    scroll_bottom();
    bsp_display_unlock();
}

void *chat_ui_begin_assistant(void)
{
    chat_ui_note_activity();
    if (!s_chat || !bsp_display_lock(1000)) return NULL;
    s_assist_buf[0] = '\0'; s_assist_len = 0;
    s_assist_lbl = add_bubble(false, COL_ASSIST, "");
    scroll_bottom();
    bsp_display_unlock();
    return s_assist_lbl;
}

void chat_ui_append_assistant(const char *delta)
{
    chat_ui_note_activity();
    if (!s_assist_lbl || !delta || !bsp_display_lock(1000)) return;

    // Consume the whole delta byte-by-byte, spilling into a fresh assistant bubble whenever the
    // current one hits ASSIST_BUBBLE_MAX_CHARS. This (a) never silently drops text the way the old
    // fixed-buffer cap did, and (b) keeps every label short → cheap to re-measure/redraw and the
    // flex-column height stays well under the lv_coord_t ceiling. We break at a space near the cap
    // so words aren't split mid-token; if there's no recent space we hard-break.
    for (size_t i = 0; delta[i]; ) {
        // If the current bubble is full, finalize it and open a new one (continuation).
        if (s_assist_len >= ASSIST_BUBBLE_MAX_CHARS) {
            // Prefer to break at the last space so the new bubble starts on a word boundary.
            size_t br = s_assist_len;
            while (br > ASSIST_BUBBLE_MAX_CHARS - 80 && br > 0 && s_assist_buf[br - 1] != ' ') br--;
            char carry[80];
            size_t carry_len = 0;
            if (br > ASSIST_BUBBLE_MAX_CHARS - 80 && br < s_assist_len) {   // found a space: move tail
                carry_len = s_assist_len - br;
                memcpy(carry, s_assist_buf + br, carry_len);
                s_assist_buf[br] = '\0';
                apply_wrapped(s_assist_lbl, s_assist_buf);                  // re-render trimmed bubble
            }
            s_assist_lbl = add_bubble(false, COL_ASSIST, "");              // continuation bubble
            memcpy(s_assist_buf, carry, carry_len);
            s_assist_len = carry_len;
            s_assist_buf[s_assist_len] = '\0';
        }
        // Append as much of the remaining delta as fits before the per-bubble cap.
        size_t room = ASSIST_BUBBLE_MAX_CHARS - s_assist_len;
        size_t take = 0;
        while (take < room && delta[i + take]) take++;
        memcpy(s_assist_buf + s_assist_len, delta + i, take);
        s_assist_len += take;
        s_assist_buf[s_assist_len] = '\0';
        i += take;
        apply_wrapped(s_assist_lbl, s_assist_buf);
    }
    scroll_bottom();
    bsp_display_unlock();
}

void chat_ui_status(const char *text)
{
    chat_ui_note_activity();
    if (!s_status || !bsp_display_lock(1000)) return;
    char clean[1024];                              // live transcript can be long
    sanitize(text ? text : "", clean, sizeof clean);
    lv_label_set_text(s_status, clean);
    lv_obj_update_layout(s_status);                // measure full text width, then position:
    lv_coord_t lw = lv_obj_get_width(s_status), bw = CHAT_W;
    lv_obj_set_x(s_status, lw > bw ? (bw - lw)      // overflow → show the tail (scroll left)
                                   : (bw - lw) / 2); // fits → center
    bsp_display_unlock();
}

void chat_ui_clear_status(void) { chat_ui_status("Ready"); }

// --- screen-power saver (power mgmt; standalone, not in the numbered phase plan) --------------
// Blank the AMOLED (brightness 0 ≈ near-zero panel draw on OLED) after a span with no activity,
// and wake on any input. Activity = touch (LVGL tracks it per input device), the PWR key (AXP2101
// PWRKEY, polled via device_tools), or any UI mutation (the chat_ui_* setters call note_activity,
// which covers PTT turns + streaming replies). A separate low-priority task polls so the I2C PWR-key
// read never runs under the LVGL lock. This is distinct from chat_ui_idle_timer (the set_timer
// countdown shown on the idle screen), below.
static uint32_t          s_idle_timeout_ms = SCREEN_IDLE_TIMEOUT_MS;
static bool              s_idle_disabled;               // "always on": never blank (configured timeout 0)
static bool              s_screen_on = true;
static bool              s_force_off_armed;             // PWR-tapped off; stays off until newer activity
static uint32_t          s_force_off_ms;                // when the force-off press happened
static volatile bool     s_alarm_active;                // a timer is ringing → it owns the screen
static lv_obj_t         *s_alarm_overlay;               // full-screen black overlay shown while ringing
static lv_obj_t         *s_alarm_ring;                  // default graphic: red ring (toggled to flash)
static lv_obj_t         *s_alarm_img;                   // user graphic (if uploaded): pulsed via img_opa
static lv_img_dsc_t      s_alarm_img_dsc;               // descriptor over s_alarm_img_buf (must persist)
static uint8_t          *s_alarm_img_buf;               // RGB565 pixels in PSRAM (freed on alarm off)
static lv_obj_t         *s_idle_overlay;                // screensaver: black overlay + pulsing image (idle)
static lv_obj_t         *s_idle_img;
static lv_img_dsc_t      s_idle_dsc;                    // descriptor over s_idle_buf (must persist)
static uint8_t          *s_idle_buf;                    // RGB565 pixels in PSRAM (freed when saver stops)
static bool              s_idle_anim_enabled;           // config (NVS): idle screensaver on/off
static lv_timer_t       *s_idle_timer;                  // drives the fade at a low rate (idle gaps)
static uint32_t          s_idle_phase_ms;               // position within the 20 s cycle
static int               s_idle_last_opa = -1;          // last opacity written (skip redundant redraws)

void chat_ui_note_activity(void) { s_last_activity_ms = now_ms(); }

// --- idle screensaver: gently pulse the uploaded image when idle (opt-in via the portal) ----------
// Disabled by default; an attention attractor ("tradeshow mode"). When enabled AND an alarm image is
// stored, the screen-power task shows this in place of blanking: a centered image on black whose
// opacity a LOW-RATE lv_timer steps through 5s blank → 5s fade-in → 5s hold → 5s fade-out → repeat
// (20s cycle). The step rate is deliberately low (~12 fps) so the priority-6 LVGL task gets idle gaps
// and never monopolizes the recursive display mutex — a continuous lv_anim at the 4 ms refresh held
// the lock back-to-back and starved the STT/UI/teardown tasks (the "Failed to acquire LVGL lock" /
// "could not lock ws-client" storm). Runs on battery too: it keeps the device awake (no light sleep),
// so it drains the battery — intentional for an always-on attractor.
#define SAVER_STEP_MS   80                                  // opacity update cadence (~12.5 fps)
#define SAVER_PHASE_MS  5000                                // each of: blank / fade-in / hold / fade-out
#define SAVER_CYCLE_MS  (4 * SAVER_PHASE_MS)                // 20 s

static void idle_saver_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_idle_img) return;
    s_idle_phase_ms += SAVER_STEP_MS;
    if (s_idle_phase_ms >= SAVER_CYCLE_MS) s_idle_phase_ms -= SAVER_CYCLE_MS;
    uint32_t p = s_idle_phase_ms;
    lv_opa_t opa;
    if      (p < SAVER_PHASE_MS)     opa = LV_OPA_TRANSP;                                       // blank
    else if (p < 2 * SAVER_PHASE_MS) opa = (p - SAVER_PHASE_MS) * 255 / SAVER_PHASE_MS;         // fade in
    else if (p < 3 * SAVER_PHASE_MS) opa = LV_OPA_COVER;                                        // hold
    else                             opa = 255 - (p - 3 * SAVER_PHASE_MS) * 255 / SAVER_PHASE_MS; // fade out
    if ((int)opa != s_idle_last_opa) {                      // skip redundant redraws during hold/blank
        lv_obj_set_style_img_opa(s_idle_img, opa, 0);
        s_idle_last_opa = (int)opa;
    }
}

static void idle_anim_stop(void)
{
    if (!s_idle_overlay && !s_idle_buf) return;            // nothing allocated
    if (bsp_display_lock(1000)) {
        if (s_idle_timer)   { lv_timer_del(s_idle_timer); s_idle_timer = NULL; }
        if (s_idle_overlay) { lv_obj_del(s_idle_overlay); s_idle_overlay = NULL; s_idle_img = NULL; }
        if (s_idle_buf)     { alarm_img_free(s_idle_buf); s_idle_buf = NULL; }  // after del → no live draw
        bsp_display_unlock();
    }
}

static bool idle_anim_start(void)
{
    if (s_idle_overlay) return true;                        // already running
    if (!alarm_img_present() || alarm_img_load(&s_idle_buf) != ESP_OK) return false;  // no image → blank
    bool ok = false;
    if (bsp_display_lock(1000)) {
        s_idle_overlay = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(s_idle_overlay);
        lv_obj_set_size(s_idle_overlay, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(s_idle_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_idle_overlay, LV_OPA_COVER, 0);
        // Click-through + non-scrollable: a touch passes to the chat beneath (so touch-to-talk still
        // works) and registers as input activity, which wakes the saver on the next screen-power tick.
        lv_obj_clear_flag(s_idle_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        s_idle_dsc.header.always_zero = 0;
        s_idle_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_idle_dsc.header.w  = ALARM_IMG_W;
        s_idle_dsc.header.h  = ALARM_IMG_H;
        s_idle_dsc.data_size = ALARM_IMG_BYTES;
        s_idle_dsc.data      = s_idle_buf;
        s_idle_img = lv_img_create(s_idle_overlay);
        lv_img_set_src(s_idle_img, &s_idle_dsc);
        lv_obj_center(s_idle_img);
        lv_obj_clear_flag(s_idle_img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_img_opa(s_idle_img, LV_OPA_TRANSP, 0);  // start blank

        s_idle_phase_ms = 0;                               // begin at the 5 s blank
        s_idle_last_opa = -1;                              // force the first opacity write
        s_idle_timer = lv_timer_create(idle_saver_tick, SAVER_STEP_MS, NULL);
        ok = (s_idle_timer != NULL);
        bsp_display_unlock();
    }
    if (!ok) {
        if (s_idle_overlay) idle_anim_stop();              // partial start → full teardown
        else if (s_idle_buf) { alarm_img_free(s_idle_buf); s_idle_buf = NULL; }  // lock failed → free buf
    }
    return ok;
}

void chat_ui_set_idle_anim_enabled(bool enabled) { s_idle_anim_enabled = enabled; }

// Enter/leave alarm mode. On: cover the UI with a black overlay holding a big red ring (outline) in
// the center, and suspend the idle power-saver so the timer task can flash the panel in time with the
// beeps. Off: remove the overlay and resume the saver (screen left on). LVGL ops → this locks; called
// from the timer task, never a handler.
void chat_ui_alarm_set(bool on)
{
    if (on) {
        idle_anim_stop();                            // a ringing timer replaces the idle screensaver
        if (bsp_display_lock(1000)) {
            if (!s_alarm_overlay) {
                s_alarm_overlay = lv_obj_create(lv_scr_act());
                lv_obj_remove_style_all(s_alarm_overlay);
                lv_obj_set_size(s_alarm_overlay, lv_pct(100), lv_pct(100));
                lv_obj_set_style_bg_color(s_alarm_overlay, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(s_alarm_overlay, LV_OPA_COVER, 0);
                lv_obj_clear_flag(s_alarm_overlay, LV_OBJ_FLAG_SCROLLABLE);

                // A user-uploaded graphic (flash) wins; otherwise the built-in red ring. The image is
                // RGB565 in PSRAM, shown centered on black and pulsed via img_opa with the beeps.
                if (alarm_img_present() && alarm_img_load(&s_alarm_img_buf) == ESP_OK) {
                    s_alarm_img_dsc.header.always_zero = 0;
                    s_alarm_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
                    s_alarm_img_dsc.header.w  = ALARM_IMG_W;
                    s_alarm_img_dsc.header.h  = ALARM_IMG_H;
                    s_alarm_img_dsc.data_size = ALARM_IMG_BYTES;
                    s_alarm_img_dsc.data      = s_alarm_img_buf;
                    lv_obj_t *img = lv_img_create(s_alarm_overlay);
                    lv_img_set_src(img, &s_alarm_img_dsc);
                    lv_obj_center(img);
                    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
                    lv_obj_set_style_img_opa(img, LV_OPA_30, 0);     // start dimmed; flash pulses it bright
                    s_alarm_img = img;
                } else {
                    lv_obj_t *ring = lv_obj_create(s_alarm_overlay); // outline-only circle (no fill)
                    lv_obj_remove_style_all(ring);
                    lv_obj_set_size(ring, 220, 220);
                    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
                    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
                    lv_obj_set_style_border_color(ring, lv_color_hex(0xFF2222), 0);
                    lv_obj_set_style_border_width(ring, 14, 0);
                    lv_obj_center(ring);
                    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
                    lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);       // start dark; flash reveals it
                    s_alarm_ring = ring;
                }
            }
            bsp_display_unlock();
        }
        s_alarm_active = true;
        // Steady brightness for the whole alarm: flash by toggling the RING (in the LVGL task), NOT by
        // toggling the panel — rapid brightness commands (0x51) share the QSPI io handle with LVGL's
        // flush and get dropped under contention (the panel just stays dark). Set it once here.
        bsp_display_brightness_set(SCREEN_ON_BRIGHTNESS);
    } else {
        s_alarm_active = false;
        if (bsp_display_lock(1000)) {
            if (s_alarm_overlay) {                         // deletes children (ring/img) too
                lv_obj_del(s_alarm_overlay);
                s_alarm_overlay = NULL; s_alarm_ring = NULL; s_alarm_img = NULL;
            }
            if (s_alarm_img_buf) { alarm_img_free(s_alarm_img_buf); s_alarm_img_buf = NULL; }  // after del → no live draw
            bsp_display_unlock();
        }
        s_screen_on = true;             // alarm ended: screen is on, resync the saver
        s_force_off_armed = false;
        chat_ui_note_activity();
    }
}

// Flash the alarm ring on/off in time with the beeps by toggling its HIDDEN flag (the panel stays at
// a steady brightness). Cheap: a flag flip + small invalidate under a short lock — the LVGL task does
// the partial redraw on its next cycle, so this never blocks the caller and never fights LVGL's flush
// for a full-screen brightness toggle. No-op if the alarm overlay isn't up. Called from the timer task.
void chat_ui_alarm_flash(bool on)
{
    if (!s_alarm_ring && !s_alarm_img) return;
    if (bsp_display_lock(80)) {
        if (s_alarm_img) {                           // user graphic: pulse opacity (never fully dark)
            lv_obj_set_style_img_opa(s_alarm_img, on ? LV_OPA_COVER : LV_OPA_30, 0);
            lv_obj_invalidate(s_alarm_img);
        } else if (s_alarm_ring) {                   // default ring: hard flash (re-check under lock)
            if (on) lv_obj_clear_flag(s_alarm_ring, LV_OBJ_FLAG_HIDDEN);
            else    lv_obj_add_flag(s_alarm_ring, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_alarm_ring);         // mark its 220x220 area dirty for the next flush
        }
        bsp_display_unlock();
    }
}

uint32_t chat_ui_touch_idle_ms(void)
{
    uint32_t ti = UINT32_MAX;
    if (bsp_display_lock(50)) { ti = lv_disp_get_inactive_time(NULL); bsp_display_unlock(); }
    return ti;
}

// Power-state hook (gates light sleep on display on/off); notified from screen_power_task.
static chat_ui_power_cb s_power_cb;
void chat_ui_set_power_cb(chat_ui_power_cb cb) { s_power_cb = cb; }

// Touch-to-talk: long-press the screen → cb(1) (start), release → cb(0) (stop). s_talk_armed gates the
// release so a quick tap (no preceding long-press) doesn't fire a spurious stop. Runs in the LVGL task
// with the display lock already held, so it only flips flags + calls the (non-blocking) app callback.
static chat_ui_talk_cb s_talk_cb;
static void           *s_talk_ctx;
static bool            s_talk_armed;
void chat_ui_set_talk_cb(chat_ui_talk_cb cb, void *ctx) { s_talk_cb = cb; s_talk_ctx = ctx; }

static void chat_ui_talk_evt_cb(lv_event_t *e)
{
    if (s_alarm_active) return;                       // a ringing timer owns the screen (tap-to-dismiss)
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        if (s_talk_armed) return;                    // already armed (e.g. both scr + s_chat fired) → once
        ESP_LOGI(TAG, "touch-to-talk: hold");
        s_talk_armed = true;
        if (s_talk_cb) s_talk_cb(1, s_talk_ctx);     // hold start → like a BOOT down
    } else if (code == LV_EVENT_RELEASED) {
        if (s_talk_armed && s_talk_cb) s_talk_cb(0, s_talk_ctx);   // release → stop + run
        s_talk_armed = false;
    }
}

// PWR (AXP2101 PWRKEY) short-press hook (used for volume down). Polled by screen_power_task.
static chat_ui_pwrkey_cb s_pwrkey_cb;
void chat_ui_set_pwrkey_cb(chat_ui_pwrkey_cb cb) { s_pwrkey_cb = cb; }

static void screen_power_task(void *arg)
{
    chat_ui_note_activity();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SCREEN_POLL_MS));

        // PWR (AXP2101 PWRKEY) short press → volume down (repurposed from the old screen toggle). It
        // also wakes the display, like any other input. A long PWR hold still powers the device off
        // (AXP2101 hardware). The screen now only blanks via the idle timeout (auto-off).
        bool pwr = device_power_key_short_press();   // always consume so the latch can't fire post-alarm
        if (s_alarm_active) continue;                // a ringing timer owns the screen (brightness flash +
                                                     // tap-to-dismiss run in timer_alert_task); skip idle/PWR
        if (pwr) {
            chat_ui_note_activity();                 // PWR press wakes the screen
            if (s_pwrkey_cb) s_pwrkey_cb();          // → volume down
        }

        // "Always on" (configured timeout = 0): never blank. Keep the panel lit (re-light it if it
        // had been off before the setting changed) and skip all idle accounting below.
        if (s_idle_disabled) {
            if (!s_screen_on) {
                s_screen_on = true;
                bsp_display_brightness_set(SCREEN_ON_BRIGHTNESS);
                if (s_power_cb) s_power_cb(true);
            }
            continue;
        }

        // Idle accounting is now driven entirely by s_last_activity_ms, which is stamped by every UI
        // mutation, PTT/PWR press, AND every touch/scroll event (chat_ui_activity_evt_cb, installed in
        // chat_ui_init). So we no longer read lv_disp_get_inactive_time under the LVGL lock — that read
        // was the sole reason this task contended bsp_display_lock(50) every 200ms, and it timed out
        // (flooding "Failed to acquire LVGL lock") exactly when the LVGL task was busy rendering a long
        // reply / heavy scroll. A busy LVGL task means the user is interacting: the opposite of idle, so
        // there is nothing to gain by reading it. Keep one cheap, non-contending peek with a generous
        // timeout purely as a backstop, and on failure treat BUSY as activity (do not blank).
        uint32_t now = now_ms();
        // Skip the peek entirely while the screensaver is up — its own (throttled) redraws touch the
        // lock, and touch activity is already stamped by the event hook, so the peek would only add
        // contention / log spam. Otherwise: a brief, generous-timeout peek near the blank threshold.
        if (!s_idle_overlay && (!s_screen_on || (now - s_last_activity_ms) >= (s_idle_timeout_ms - SCREEN_POLL_MS))) {
            if (bsp_display_lock(200)) {
                uint32_t ti = lv_disp_get_inactive_time(NULL);
                bsp_display_unlock();
                if (ti < (now - s_last_activity_ms)) s_last_activity_ms = now - ti;   // touch is newer
            } else {
                s_last_activity_ms = now;   // LVGL busy → treat as activity, never blank mid-render
            }
        }
        uint32_t act_age = now - s_last_activity_ms;                  // ms since last activity of any kind
        uint32_t idle = act_age;

        if (s_force_off_armed && idle < (now - s_force_off_ms))
            s_force_off_armed = false;                               // activity after the press → unarm

        bool want_on = s_force_off_armed ? false : (idle < s_idle_timeout_ms);
        if (want_on != s_screen_on) {
            if (want_on) {                          // → on (woke from blank or screensaver)
                idle_anim_stop();                   // remove the screensaver if it was showing
                s_screen_on = true;
                bsp_display_brightness_set(SCREEN_ON_BRIGHTNESS);
                ESP_LOGI(TAG, "screen on (idle=%ums)", (unsigned)idle);
                if (s_power_cb) s_power_cb(true);   // back to active power state
            } else if (s_idle_anim_enabled && idle_anim_start()) {
                // Idle + screensaver enabled + image present: pulse the image instead of blanking. Keep
                // the panel lit + the system awake (the animation needs the CPU) → no power_cb(false).
                s_screen_on = false;
                bsp_display_brightness_set(SCREEN_ON_BRIGHTNESS);
                ESP_LOGI(TAG, "screen idle → screensaver (idle=%ums)", (unsigned)idle);
            } else {                                // → off (blank)
                s_screen_on = false;
                bsp_display_brightness_set(0);
                ESP_LOGI(TAG, "screen off (idle=%ums%s)", (unsigned)idle,
                         s_force_off_armed ? ", PWR-off" : "");
                if (s_power_cb) s_power_cb(false);  // gate light sleep on display state
            }
        }
    }
}

void chat_ui_set_screen_timeout_s(int seconds)
{
    if (seconds <= 0) {                                // 0 = always on (never blank)
        s_idle_disabled = true;
    } else {
        s_idle_disabled = false;
        s_idle_timeout_ms = (uint32_t)seconds * 1000;
    }
}

void chat_ui_screen_power_start(int idle_timeout_s)
{
    chat_ui_set_screen_timeout_s(idle_timeout_s);      // 0 = always on; >0 = blank after N seconds
    chat_ui_note_activity();
    s_screen_on = true;
    s_force_off_armed = false;
    xTaskCreate(screen_power_task, "scrnpwr", 4096, NULL, 3, NULL);
    if (s_idle_disabled)
        ESP_LOGI(TAG, "screen-power saver: always on (no auto-blank)");
    else
        ESP_LOGI(TAG, "screen-power saver: blank after %us idle (wake on touch / PWR key / activity)",
                 (unsigned)(s_idle_timeout_ms / 1000));
}

// --- later phases ---
void chat_ui_show_qr(const char *data)            { (void)data; }          // P6
void chat_ui_idle_timer(int s, const char *label) { (void)s; (void)label; } // P7: set_timer countdown

void chat_ui_prompt(const char *message, const cJSON *response_schema,
                    int64_t expires_at, chat_ui_answer_cb cb, void *ctx)
{
    (void)message; (void)response_schema; (void)expires_at; (void)cb; (void)ctx;
    ESP_LOGW(TAG, "chat_ui_prompt not implemented yet");   // P6
}
