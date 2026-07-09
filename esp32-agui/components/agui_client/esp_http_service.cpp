// EspHttpService — see esp_http_service.h. Reuses the open/write/fetch/read SSE loop that the
// pre-rearchitecture agui_client.c used (esp_http_client + esp_crt_bundle, PSRAM read buffer).
#include "esp_http_service.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "core/error.h"   // agui::AgentError

namespace aguidev {

static const char* TAG = "esp_http_svc";
static constexpr int kReadChunk = 1024;

static esp_http_client_method_t mapMethod(agui::HttpMethod m) {
    switch (m) {
        case agui::HttpMethod::GET:    return HTTP_METHOD_GET;
        case agui::HttpMethod::POST:   return HTTP_METHOD_POST;
        case agui::HttpMethod::PUT:    return HTTP_METHOD_PUT;
        case agui::HttpMethod::DELETE: return HTTP_METHOD_DELETE;
        case agui::HttpMethod::PATCH:  return HTTP_METHOD_PATCH;
    }
    return HTTP_METHOD_POST;
}

void EspHttpService::sendRequest(const agui::HttpRequest& request, agui::HttpResponseCallback onResponse,
                                 agui::HttpErrorCallback onError) {
    // HttpAgent only uses the SSE path; a non-streaming impl isn't needed on-device.
    (void)request;
    (void)onResponse;
    if (onError) {
        onError(agui::AgentError::network(agui::ErrorCode::NetworkError,
                                          "EspHttpService::sendRequest not implemented"));
    }
}

void EspHttpService::sendSseRequest(const agui::HttpRequest& request, agui::SseDataCallback onData,
                                    agui::SseCompleteCallback onComplete, agui::HttpErrorCallback onError) {
    // Register a cancel flag for this run; remove it on every exit path.
    auto flag = std::make_shared<std::atomic<bool>>(false);
    if (!request.cancelKey.empty()) {
        std::lock_guard<std::mutex> lk(m_cancelMutex);
        m_cancel[request.cancelKey] = flag;
    }
    struct Unregister {
        EspHttpService* self;
        std::string key;
        ~Unregister() {
            if (key.empty()) return;
            std::lock_guard<std::mutex> lk(self->m_cancelMutex);
            self->m_cancel.erase(key);
        }
    } unreg{this, request.cancelKey};

    esp_http_client_config_t hcfg = {};
    hcfg.url = request.url.c_str();
    hcfg.method = mapMethod(request.method);
    hcfg.timeout_ms = request.timeoutMs > 0 ? request.timeoutMs : 45000;
    hcfg.buffer_size = 2048;
    hcfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) {
        if (onError) {
            onError(agui::AgentError::network(agui::ErrorCode::NetworkConnectionFailed,
                                              "esp_http_client_init failed"));
        }
        return;
    }

    // Body is JSON; the SDK's builder headers (Accept, Authorization) ride in request.headers.
    esp_http_client_set_header(client, "Content-Type", "application/json");
    for (const auto& kv : request.headers) {
        esp_http_client_set_header(client, kv.first.c_str(), kv.second.c_str());
    }

    const int bodyLen = static_cast<int>(request.body.size());
    esp_err_t oe = esp_http_client_open(client, bodyLen);
    if (oe != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(oe));
        esp_http_client_cleanup(client);
        if (onError) {
            onError(agui::AgentError::network(agui::ErrorCode::NetworkConnectionFailed,
                                              std::string("connect failed: ") + esp_err_to_name(oe)));
        }
        return;
    }

    for (int off = 0; off < bodyLen;) {
        int w = esp_http_client_write(client, request.body.data() + off, bodyLen - off);
        if (w < 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            if (onError) {
                onError(agui::AgentError::network(agui::ErrorCode::NetworkError, "request write failed"));
            }
            return;
        }
        off += w;
    }

    esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);

    agui::HttpResponse finalResp;
    finalResp.statusCode = status;

    if (status < 200 || status >= 300) {
        // Non-2xx: don't feed the error body to the SSE parser — let handleStreamComplete()
        // turn the non-success status into an onError for the run.
        ESP_LOGW(TAG, "HTTP %d from agent", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (onComplete) onComplete(finalResp);
        return;
    }

    char* chunk = static_cast<char*>(heap_caps_malloc(kReadChunk, MALLOC_CAP_SPIRAM));
    if (!chunk) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (onError) {
            onError(agui::AgentError::network(agui::ErrorCode::NetworkError, "no PSRAM for SSE read buffer"));
        }
        return;
    }

    bool cancelled = false;
    for (;;) {
        if (flag->load()) {
            cancelled = true;
            break;
        }
        int n = esp_http_client_read(client, chunk, kReadChunk);
        if (n <= 0) break;   // EOF / stream closed by server / read end
        if (onData) {
            agui::HttpResponse resp;
            resp.statusCode = status;
            resp.content.assign(chunk, static_cast<size_t>(n));   // latest chunk only; SDK SseParser accumulates
            onData(resp);
        }
    }
    heap_caps_free(chunk);

    finalResp.cancelled = cancelled;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (onComplete) onComplete(finalResp);
}

void EspHttpService::cancelRequest(const std::string& requestKey) {
    std::lock_guard<std::mutex> lk(m_cancelMutex);
    auto it = m_cancel.find(requestKey);
    if (it != m_cancel.end()) it->second->store(true);
}

}  // namespace aguidev
