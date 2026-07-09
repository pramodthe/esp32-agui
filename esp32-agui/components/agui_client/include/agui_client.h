// agui_client — on-device AG-UI client.
//
// POST RunAgentInput → consume SSE event stream. Ported from the ag-ui C++ SDK
// (libcurl→esp_http_client, nlohmann→cJSON). Three device extensions: per-run ambient
// `context`, Interrupt→resume, client-tool dispatch. v1 skips STATE_*/JSON-Patch.
// See docs/esp32-agui-plan.md §5.2 / §6.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;
    const char *reason;
    const char *message;
    const char *tool_call_id;
    cJSON      *response_schema;
    int64_t     expires_at;
} agui_interrupt_t;

typedef struct {
    void (*on_run_started)(void *ctx);
    void (*on_text_delta)(const char *delta, void *ctx);   // TEXT_MESSAGE_CONTENT
    void (*on_text_end)(void *ctx);
    void (*on_reasoning)(const char *delta, bool active, void *ctx);
    void (*on_tool_call)(const char *id, const char *name, const char *args_json, void *ctx);
    void (*on_interrupt)(const agui_interrupt_t *it, void *ctx);  // RUN_FINISHED outcome=interrupt
    void (*on_run_finished)(void *ctx);
    void (*on_error)(const char *msg, void *ctx);
} agui_handlers_t;

typedef struct {
    const char *endpoint;
    const char *auth_bearer;
    const char *thread_id;
} agui_cfg_t;

// One-time init.
esp_err_t agui_client_init(void);

esp_err_t agui_run(const agui_cfg_t *cfg, const char *user_text,
                   const cJSON *context,   // [{description,value}] ambient
                   const cJSON *tools,     // advertised client tools
                   const cJSON *resume,    // NULL unless resuming an interrupt
                   const agui_handlers_t *handlers, void *ctx);

esp_err_t agui_tool_result(const char *tool_call_id, const cJSON *result);

// Abort the in-flight agent run (if any) — for PTT barge-in. Callable from ANY task (e.g. a button
// cb): it takes NO agui lock, only flips the SDK's per-run atomic cancel flag that the SSE read loop
// polls, so it won't deadlock against the blocking agui_run() that holds s_lock. No-op if no run is
// active. The aborted run surfaces as on_error("...cancelled...") — the caller must distinguish that
// deliberate cancel from a real error (see s_aborting in main).
void agui_abort(void);

// Drop a trailing PARTIAL assistant message from SDK history (the half-streamed reply left by an
// abort) so it can't poison the next run's context. No-op if history is empty or its tail isn't an
// assistant message. Takes s_lock → call only BETWEEN runs (ptt_task), never from a handler.
void agui_drop_partial_assistant(void);

#ifdef __cplusplus
}
#endif
