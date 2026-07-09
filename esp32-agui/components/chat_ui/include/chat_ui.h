// chat_ui — LVGL UI: chat list, ephemeral status, interrupt prompt, idle timer.
//
// Renders TEXT_* as chat bubbles, REASONING_*/TOOL_CALL_*/RUN_* as ephemeral status,
// and response_schema-driven interrupt prompts (touch). UI cues borrowed from
// app-pixels/ai-assistant-claude (status pill, VU meter, "You:/AI:" labels). Font
// management: LVGL fonts in flash + fallback glyph. See docs/esp32-agui-plan.md §5.4 / §9.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-time init (BSP display/touch + LVGL bring-up happens here in P3).
esp_err_t chat_ui_init(void);

void  chat_ui_add_user(const char *text);
// Returns an opaque handle to the assistant bubble (lv_obj_t* under the hood).
void *chat_ui_begin_assistant(void);
void  chat_ui_append_assistant(const char *delta);

void  chat_ui_status(const char *text);   // ephemeral
void  chat_ui_clear_status(void);
void  chat_ui_show_qr(const char *data);   // lv_qrcode
void  chat_ui_idle_timer(int seconds_left, const char *label);

// Screen-power saver: blank the AMOLED (brightness 0) after `idle_timeout_s` of no touch /
// PTT / UI activity, and wake it on any of those. Pass 0 for "always on" (never blank); >0 sets
// the timeout in seconds. (The PWR button no longer toggles the screen — it's volume-down now, so
// the timeout is the only auto-off.) Starts a small background task; call once after chat_ui_init().
void  chat_ui_screen_power_start(int idle_timeout_s);
// Update the screen blank timeout live (no task restart). 0 = always on; >0 = seconds. Used by the
// boot config load; safe to call from any task.
void  chat_ui_set_screen_timeout_s(int seconds);
// Idle screensaver: when enabled, gently pulse the uploaded alarm image while idle (in place of
// blanking) — 5s blank, fade in 5s, hold 5s, fade out 5s, repeat (20s cycle). Runs on battery too
// (keeps the device awake → drains it; intentional attractor). Needs an alarm image + screen timeout
// > 0. Default off. Safe to call from any task.
void  chat_ui_set_idle_anim_enabled(bool enabled);
// Mark user/agent activity so the screen-power saver keeps the display awake. The UI mutators call
// this internally; call it too from external input sources (e.g. button presses).
void  chat_ui_note_activity(void);

// Alarm mode: while a timer is ringing, the timer task owns the screen (brightness flash). Setting
// this suspends the idle power-saver so it doesn't fight the flashing; pass false to resume (the saver
// treats the screen as on + active). Lets an alert flash the panel without the saver blanking it.
void     chat_ui_alarm_set(bool on);
// Flash the alarm ring on/off, in time with the beeps. Toggles the ring's visibility inside the
// overlay (the panel stays at a steady brightness while ringing — toggling the panel brightness gets
// starved by LVGL's lock/flush). Cheap; no-op if no alarm is active. Called from the timer task.
void     chat_ui_alarm_flash(bool on);
// ms since the last touch (LVGL input inactivity — object-independent, so a tap anywhere counts),
// or UINT32_MAX if the LVGL lock is momentarily busy. Used to detect a tap-to-dismiss during an alarm.
uint32_t chat_ui_touch_idle_ms(void);
// UI liveness counter, bumped once a second from inside the LVGL task. If successive reads are
// equal, the LVGL task is wedged (holding the display mutex) — surface it in the heartbeat.
uint32_t chat_ui_ui_ticks(void);

// Power-state hook: called from the screen-power task whenever the display turns on/off. Lets the app
// gate power management on display state (e.g. allow light sleep only while the display is off).
typedef void (*chat_ui_power_cb)(bool display_on);
void     chat_ui_set_power_cb(chat_ui_power_cb cb);

// Touch-to-talk hook: a long-press anywhere on the screen fires cb(1) (hold start); the release fires
// cb(0) (stop). Mirrors a BOOT-button down/up so the app can drive the same PTT path. Runs in the LVGL
// task with the display lock held — the callback MUST be non-blocking (no locks / I2C / waits).
typedef void (*chat_ui_talk_cb)(int ev, void *ctx);   // ev: 1 = press (hold start), 0 = release
void     chat_ui_set_talk_cb(chat_ui_talk_cb cb, void *ctx);

// PWR-key hook: the AXP2101 PWRKEY short-press (polled by the screen-power task) fires this instead of
// toggling the screen. Used for volume-down. Runs on the screen-power task — keep it non-blocking.
typedef void (*chat_ui_pwrkey_cb)(void);
void     chat_ui_set_pwrkey_cb(chat_ui_pwrkey_cb cb);

// Interrupt prompt: build widgets from response_schema; answer returned via callback.
typedef void (*chat_ui_answer_cb)(const cJSON *answer, void *ctx);
void  chat_ui_prompt(const char *message, const cJSON *response_schema,
                     int64_t expires_at, chat_ui_answer_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
