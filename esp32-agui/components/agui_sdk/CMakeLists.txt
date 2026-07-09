# Vendored AG-UI community C++ SDK (ag-ui-protocol/ag-ui sdks/community/c++, MIT) — see PATCHES.md.
# The libcurl transport (src/http/http_service.cpp) is removed; the agui_client component supplies an
# IHttpService backed by esp_http_client. nlohmann/json comes from the johboh/nlohmann-json managed
# dependency declared in idf_component.yml (auto-added to this component's requirements).
idf_component_register(
    SRCS
        "src/core/error.cpp"
        "src/core/event.cpp"
        "src/core/event_verifier.cpp"
        "src/core/logger.cpp"
        "src/core/session_types.cpp"
        "src/core/state.cpp"
        "src/core/subscriber.cpp"
        "src/core/uuid.cpp"
        "src/middleware/middleware.cpp"
        "src/stream/sse_parser.cpp"
        "src/apply/apply.cpp"
        "src/agent/http_agent.cpp"
    INCLUDE_DIRS "src"
    # Public: the SDK's public headers (session_types.h, event.h, ...) include <nlohmann/json.hpp>,
    # so the dependency must propagate to anything that REQUIRES agui_sdk (e.g. agui_client).
    REQUIRES johboh__nlohmann-json)

# Upstream targets C++17; pin it here even if the IDF default toolchain standard is newer.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
