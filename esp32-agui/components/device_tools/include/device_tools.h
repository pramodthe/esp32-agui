// device_tools — tool registry + implementations + ambient-context provider.
//
// Ambient context per run = motion (QMI8658) + battery (AXP2101) + local_time (PCF85063).
// Builtin client tools: set_timer, set_alarm, show_qr. Dispatched from AG-UI TOOL_CALL_*.
// See docs/esp32-agui-plan.md §5.3 / §6.
#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-time init: registers builtin tools.
esp_err_t device_tools_init(void);

cJSON *device_context_build(void);   // [{description,value}] ambient context
cJSON *device_tools_manifest(void);  // JSON-schema list for RunAgentInput.tools

// PWR button (AXP2101 PWRKEY) short-press since the last call (latched IRQ, write-1-to-clear).
// A long press is a hardware power-off handled by the PMIC, so only short presses are reported.
// Used by the chat_ui screen-power saver to wake the display. False if the AXP2101 is unavailable.
bool device_power_key_short_press(void);

// True if running on battery (no USB/VBUS power). Gates idle WiFi-off (plugged-in stays connected).
bool device_tools_on_battery(void);

// Battery gauge for UI (clock page). Returns false if PMIC/gauge unavailable.
// percent_out: 0..100; plugged_out: USB present; status: short human string (optional).
bool device_tools_battery_read(int *percent_out, bool *plugged_out, char *status, size_t status_len);

// A tool implementation: parse args, produce a result JSON (caller owns *result).
typedef esp_err_t (*device_tool_fn)(const cJSON *args, cJSON **result);

void device_tools_register(const char *name, const cJSON *schema, device_tool_fn fn);

// Dispatch a tool call by name (returns ESP_ERR_NOT_FOUND if unknown).
esp_err_t device_tools_dispatch(const char *name, const cJSON *args, cJSON **result);

// True if `name` is a registered client tool (device executes it + owes a result). False for
// server/agent tools, which the device must NOT capture or answer.
bool device_tools_is_client(const char *name);

// set_timer accessors (the tool itself never touches the UI to avoid a chat_ui<->device_tools cycle):
int  device_tools_timer_remaining(void);                  // seconds left on the active timer, 0 if none
bool device_tools_timer_take_fired(char *label, size_t n); // true ONCE after a timer elapses (copies label)
// Queue signaled (1 item) when a set_timer elapses — block on it instead of polling so the CPU can
// light-sleep until the deadline. Drain with device_tools_timer_take_fired().
QueueHandle_t device_tools_timer_queue(void);

// show_image: device_tools owns the AG-UI tool registration; chat_ui owns download/display.
// Main wires the handler after chat_ui_init() so we avoid a circular component dependency.
typedef esp_err_t (*device_show_image_fn)(const char *url);
void device_tools_set_show_image_handler(device_show_image_fn fn);

#ifdef __cplusplus
}
#endif
