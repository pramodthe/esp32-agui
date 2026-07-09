// agui_client — extern "C" shim over the vendored AG-UI C++ SDK (component agui_sdk).
// See include/agui_client.h. Replaces the old hand-rolled C SSE/strcmp router: events are now
// decoded by the SDK's EventParser into typed Event objects and dispatched through an HttpAgent +
// IAgentSubscriber. A single CHandlersSubscriber adapts the SDK's typed callbacks back to the
// existing agui_handlers_t C callbacks so main.c is unchanged. EspHttpService provides transport.
//
// One run at a time (s_lock) → sequential TLS. The HttpAgent persists across turns and owns the
// conversation history (messages()); we keep the threadId for continuity.

#include "agui_client.h"
#include "app_cfg.h"
#include "esp_http_service.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"

#include "agent/http_agent.h"
#include "core/event.h"
#include "core/subscriber.h"
#include "core/session_types.h"

static const char* TAG = "agui_client";

namespace {

SemaphoreHandle_t s_lock = nullptr;       // serializes runs (sequential TLS)
std::unique_ptr<agui::HttpAgent> s_agent;
std::string s_thread_id;
std::string s_cur_endpoint;               // cached to detect a reconfigure (endpoint/token change)
std::string s_cur_token;

std::string gen_thread_id() {
    unsigned a = esp_random(), b = esp_random();
    char buf[40];
    std::snprintf(buf, sizeof buf, "thread_%08x%08x", a, b);
    return buf;
}

// Adapter: vendored SDK typed events -> agui_handlers_t C callbacks. Bound (handlers+ctx) per run.
// All callbacks fire synchronously on the run task (PTT task), exactly like the old C router.
class CHandlersSubscriber : public agui::IAgentSubscriber {
public:
    void bind(const agui_handlers_t* h, void* ctx) {
        h_ = h;
        ctx_ = ctx;
        reported_error_ = false;
    }
    bool reportedError() const { return reported_error_; }

    agui::AgentStateMutation onRunStarted(const agui::RunStartedEvent&,
                                          const agui::AgentSubscriberParams&) override {
        if (h_ && h_->on_run_started) h_->on_run_started(ctx_);
        return {};
    }

    agui::AgentStateMutation onTextMessageContent(const agui::TextMessageContentEvent& e, const std::string&,
                                                  const agui::AgentSubscriberParams&) override {
        if (h_ && h_->on_text_delta) h_->on_text_delta(e.delta.c_str(), ctx_);
        return {};
    }

    agui::AgentStateMutation onTextMessageEnd(const agui::TextMessageEndEvent&,
                                              const agui::AgentSubscriberParams&) override {
        if (h_ && h_->on_text_end) h_->on_text_end(ctx_);
        return {};
    }

    // Reasoning (current protocol) -> on_reasoning(delta, active)
    agui::AgentStateMutation onReasoningStart(const agui::ReasoningStartEvent&,
                                              const agui::AgentSubscriberParams&) override {
        if (h_ && h_->on_reasoning) h_->on_reasoning(nullptr, true, ctx_);
        return {};
    }
    agui::AgentStateMutation onReasoningMessageStart(const agui::ReasoningMessageStartEvent&,
                                                     const agui::AgentSubscriberParams&) override {
        if (h_ && h_->on_reasoning) h_->on_reasoning(nullptr, true, ctx_);
        return {};
    }
    agui::AgentStateMutation onReasoningMessageContent(const agui::ReasoningMessageContentEvent& e, const std::string&,
                                                       const agui::AgentSubscriberParams&) override {
        if (h_ && h_->on_reasoning && !e.delta.empty()) h_->on_reasoning(e.delta.c_str(), true, ctx_);
        return {};
    }
    agui::AgentStateMutation onReasoningMessageEnd(const agui::ReasoningMessageEndEvent&,
                                                   const agui::AgentSubscriberParams&) override {
        if (h_ && h_->on_reasoning) h_->on_reasoning(nullptr, false, ctx_);
        return {};
    }
    agui::AgentStateMutation onReasoningEnd(const agui::ReasoningEndEvent&,
                                            const agui::AgentSubscriberParams&) override {
        if (h_ && h_->on_reasoning) h_->on_reasoning(nullptr, false, ctx_);
        return {};
    }

    // Tool calls -> on_tool_call(id, name, args). Args accumulated by the EventHandler; emit at END.
    agui::AgentStateMutation onToolCallStart(const agui::ToolCallStartEvent& e,
                                             const agui::AgentSubscriberParams&) override {
        tool_id_ = e.toolCallId;
        tool_name_ = e.toolCallName;
        tool_args_.clear();
        return {};
    }
    agui::AgentStateMutation onToolCallArgs(const agui::ToolCallArgsEvent&, const std::string& buffer,
                                            const agui::AgentSubscriberParams&) override {
        tool_args_ = buffer;   // accumulated args so far
        return {};
    }
    agui::AgentStateMutation onToolCallEnd(const agui::ToolCallEndEvent&,
                                           const agui::AgentSubscriberParams&) override {
        if (h_ && h_->on_tool_call)
            h_->on_tool_call(tool_id_.c_str(), tool_name_.c_str(),
                             tool_args_.empty() ? "{}" : tool_args_.c_str(), ctx_);
        return {};
    }

    agui::AgentStateMutation onRunFinished(const agui::RunFinishedEvent& e,
                                           const agui::AgentSubscriberParams&) override {
        if (e.isInterrupt() && h_ && h_->on_interrupt) {
            // Surface each interrupt to the C handler (P6 renders the prompt + resume run).
            for (const auto& it : e.interrupts) {
                agui_interrupt_t ci = {};
                ci.id = it.id.c_str();
                ci.reason = it.reason.c_str();
                ci.message = it.message ? it.message->c_str() : nullptr;
                ci.tool_call_id = it.toolCallId ? it.toolCallId->c_str() : nullptr;
                ci.response_schema = nullptr;   // P6: convert it.responseSchema (nlohmann) -> cJSON
                ci.expires_at = it.expiresAt.value_or(0);
                h_->on_interrupt(&ci, ctx_);
            }
        }
        if (h_ && h_->on_run_finished) h_->on_run_finished(ctx_);
        return {};
    }

    agui::AgentStateMutation onRunError(const agui::RunErrorEvent& e,
                                        const agui::AgentSubscriberParams&) override {
        if (h_ && h_->on_error) h_->on_error(e.message.c_str(), ctx_);
        reported_error_ = true;
        return {};
    }

private:
    const agui_handlers_t* h_ = nullptr;
    void* ctx_ = nullptr;
    bool reported_error_ = false;
    std::string tool_id_, tool_name_, tool_args_;
};

std::shared_ptr<CHandlersSubscriber> s_adapter;

// (Re)build the agent when the endpoint/token changes (or first run). A rebuild starts a fresh
// conversation (new history + threadId) — matching a runtime reconfigure. Returns false on failure.
bool ensure_agent(const agui_cfg_t* cfg) {
    std::string endpoint = cfg->endpoint ? cfg->endpoint : "";
    std::string token = (cfg->auth_bearer && cfg->auth_bearer[0]) ? cfg->auth_bearer : "";

    bool need_rebuild = !s_agent || endpoint != s_cur_endpoint || token != s_cur_token;
    if (need_rebuild) {
        agui::HttpAgent::Builder builder = agui::HttpAgent::builder();
        builder.withUrl(endpoint).withHeader("Accept", "text/event-stream").withTimeout(45);
        if (!token.empty()) builder.withBearerToken(token);
        s_agent = builder.build();
        if (!s_agent) return false;
        s_agent->setHttpService(std::make_unique<aguidev::EspHttpService>());
        if (!s_adapter) s_adapter = std::make_shared<CHandlersSubscriber>();
        s_agent->subscribe(s_adapter);
        s_cur_endpoint = endpoint;
        s_cur_token = token;
        s_thread_id = gen_thread_id();   // fresh endpoint => fresh thread
    }
    if (cfg->thread_id && cfg->thread_id[0]) s_thread_id = cfg->thread_id;   // explicit override wins
    if (s_thread_id.empty()) s_thread_id = gen_thread_id();
    return true;
}

// Convert a cJSON ambient-context array [{description,value}] into RunAgentParams (P5).
void add_context(agui::RunAgentParams& params, const cJSON* context) {
    if (!context || !cJSON_IsArray(context)) return;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, context) {
        const cJSON* d = cJSON_GetObjectItemCaseSensitive(item, "description");
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(item, "value");
        agui::Context c;
        if (cJSON_IsString(d) && d->valuestring) c.description = d->valuestring;
        if (cJSON_IsString(v) && v->valuestring) c.value = v->valuestring;
        params.addContext(c);
    }
}

// Convert a cJSON tool manifest [{name, description, parameters(JSON-Schema)}] into
// RunAgentParams.tools so they serialize into RunAgentInput.tools (P7 client tools). Mirrors
// add_context(). `parameters` is passed through verbatim as the JSON-Schema object.
void add_tools(agui::RunAgentParams& params, const cJSON* tools) {
    if (!tools || !cJSON_IsArray(tools)) return;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, tools) {
        const cJSON* n = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON* d = cJSON_GetObjectItemCaseSensitive(item, "description");
        const cJSON* p = cJSON_GetObjectItemCaseSensitive(item, "parameters");
        if (!cJSON_IsString(n) || !n->valuestring) continue;        // name is required

        agui::Tool t;
        t.name = n->valuestring;
        if (cJSON_IsString(d) && d->valuestring) t.description = d->valuestring;

        // No cJSON->nlohmann bridge exists; round-trip the schema subtree through a string.
        // Default to an empty-object schema if absent/unparsable (a no-throw parse can't abort the turn).
        t.parameters = nlohmann::json::object();
        if (p) {
            char* s = cJSON_PrintUnformatted(p);
            if (s) {
                auto parsed = nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
                if (!parsed.is_discarded()) t.parameters = std::move(parsed);
                cJSON_free(s);
            }
        }
        params.addTool(t);
    }
}

}  // namespace

extern "C" esp_err_t agui_client_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_thread_id.empty()) s_thread_id = gen_thread_id();
    ESP_LOGI(TAG, "init (thread %s)", s_thread_id.c_str());
    return ESP_OK;
}

extern "C" esp_err_t agui_run(const agui_cfg_t* cfg, const char* user_text,
                              const cJSON* context, const cJSON* tools, const cJSON* resume,
                              const agui_handlers_t* handlers, void* ctx) {
    (void)resume;   // interrupt resume payload — wired in P6
    if (!cfg || !cfg->endpoint || !handlers) return ESP_ERR_INVALID_ARG;
    if (!s_lock) agui_client_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t ret = ESP_FAIL;
    try {
        if (!ensure_agent(cfg)) {
            if (handlers->on_error) handlers->on_error("agent init failed", ctx);
            xSemaphoreGive(s_lock);
            return ESP_FAIL;
        }
        s_adapter->bind(handlers, ctx);

        // The HttpAgent owns history; append the new user turn so it persists across runs.
        if (!resume && user_text && user_text[0]) {
            s_agent->addMessage(agui::Message::create(agui::MessageRole::User, user_text));
        }

        agui::RunAgentParams params;
        params.threadId = s_thread_id;
        add_context(params, context);
        add_tools(params, tools);          // P7: advertise device client tools in RunAgentInput.tools

        bool ok = true;
        std::string errMsg;
        s_agent->runAgent(
            params,
            [&](const agui::RunAgentResult&) { ok = true; },
            [&](const std::string& e) {
                ok = false;
                errMsg = e;
            });

        // A transport/HTTP error reaches us via the agent's onError (not a RUN_ERROR event), so it
        // may not have hit on_error yet — surface it once, without double-reporting RUN_ERROR.
        if (!ok && !s_adapter->reportedError() && handlers->on_error) {
            handlers->on_error(errMsg.empty() ? "run failed" : errMsg.c_str(), ctx);
        }
        ret = ok ? ESP_OK : ESP_FAIL;
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "exception in run: %s", e.what());
        if (handlers->on_error) handlers->on_error(e.what(), ctx);
        ret = ESP_FAIL;
    } catch (...) {
        ESP_LOGE(TAG, "unknown exception in run");
        if (handlers->on_error) handlers->on_error("agui run failed", ctx);
        ret = ESP_FAIL;
    }

    xSemaphoreGive(s_lock);
    return ret;
}

// Append a client-tool RESULT to the agent's history so the next (continuation) run carries it.
// The assistant(tool_calls) message is ALREADY in history (the SDK's EventHandler reconstructs it
// during the run and never resets it), so we append exactly ONE Tool-role message keyed by the
// matching toolCallId. Mutates s_agent, so it takes s_lock — it MUST be called BETWEEN runs from
// run_agent_turn (ptt_task), never from a handler (which runs inside agui_run holding s_lock).
extern "C" esp_err_t agui_tool_result(const char* tool_call_id, const cJSON* result) {
    if (!tool_call_id || !tool_call_id[0]) return ESP_ERR_INVALID_ARG;
    if (!s_lock) agui_client_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_FAIL;
    try {
        if (s_agent) {
            // Tool message content is a string. A plain-string result is used as-is; structured
            // results are JSON-serialized.
            std::string content;
            if (result && cJSON_IsString(result) && result->valuestring) {
                content = result->valuestring;
            } else if (result) {
                char* s = cJSON_PrintUnformatted(result);
                if (s) { content = s; cJSON_free(s); }
            }
            s_agent->addMessage(
                agui::Message::create(agui::MessageRole::Tool, content, "", tool_call_id));
            ret = ESP_OK;
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tool_result failed: %s", e.what());
    } catch (...) {}
    xSemaphoreGive(s_lock);
    return ret;
}

// PTT barge-in: abort the in-flight run from another task. No s_lock (the blocked agui_run holds it).
// cancelRun() flips an atomic the SSE read loop polls and is internally serialised on m_currentRunKey.
extern "C" void agui_abort(void) {
    if (s_agent) s_agent->cancelRun();
}

// After an abort, the SDK may hold a half-streamed assistant message (appended at TEXT_MESSAGE_START).
// Pop it so it doesn't leak into the next run's context. Only ever called from the abort path, so a
// trailing assistant message here is always the aborted partial (a completed reply is never dropped).
extern "C" void agui_drop_partial_assistant(void) {
    if (!s_lock) agui_client_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    try {
        if (s_agent) {
            std::vector<agui::Message> msgs = s_agent->messages();   // copy (messages() returns a const ref)
            if (!msgs.empty() && msgs.back().role() == agui::MessageRole::Assistant) {
                msgs.pop_back();
                s_agent->setMessages(msgs);                          // replace history (reindexes internally)
                ESP_LOGI(TAG, "dropped partial assistant message after abort");
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "drop_partial failed: %s", e.what());
    } catch (...) {}
    xSemaphoreGive(s_lock);
}
