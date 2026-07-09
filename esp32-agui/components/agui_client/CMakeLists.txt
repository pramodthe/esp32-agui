idf_component_register(
    SRCS "agui_client.cpp" "esp_http_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES json esp_http_client esp-tls esp_hw_support app_cfg agui_sdk)
# extern "C" shim + IAgentSubscriber adapter over the vendored AG-UI C++ SDK (agui_sdk).
# EspHttpService implements the SDK's IHttpService over esp_http_client + esp-tls (crt bundle).
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
