// EspHttpService — ESP-IDF transport for the vendored AG-UI SDK.
//
// Implements agui::IHttpService over esp_http_client, replacing the SDK's dropped libcurl
// transport (agui_sdk/src/http/http_service.cpp). SSE streaming runs synchronously on the
// caller's task (the PTT task): onData/onComplete/onError fire on that same thread, so the
// SDK event handler and our subscriber run there too — no cross-thread UI calls.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "http/http_service.h"   // agui::IHttpService (vendored SDK header)

namespace aguidev {

class EspHttpService : public agui::IHttpService {
public:
    void sendRequest(const agui::HttpRequest& request, agui::HttpResponseCallback onResponse,
                     agui::HttpErrorCallback onError) override;
    void sendSseRequest(const agui::HttpRequest& request, agui::SseDataCallback onData,
                        agui::SseCompleteCallback onComplete, agui::HttpErrorCallback onError) override;
    void cancelRequest(const std::string& requestKey) override;

private:
    // cancelKey (runId) -> flag the read loop polls; set by cancelRequest() (possibly another task).
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> m_cancel;
    std::mutex m_cancelMutex;
};

}  // namespace aguidev
