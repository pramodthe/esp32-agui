// Companion page shell: Eyes (face) vs Chat. The on-device clock page was removed —
// the agent answers time queries, so a short tap only toggles Eyes <-> Chat.
#include "companion_pages.h"

#include <string.h>

#include "esp_log.h"
#include "face_engine.h"

static const char *TAG = "companion";

static companion_page_t s_page = COMPANION_PAGE_EYES;

static void apply_visibility(void)
{
    bool eyes = (s_page == COMPANION_PAGE_EYES);
    // Chat = face hidden, so the chat list underneath shows through.
    face_engine_set_visible(eyes);
    face_engine_set_active(eyes);
}

void companion_pages_create(lv_obj_t *scr)
{
    (void)scr;
    s_page = COMPANION_PAGE_EYES;
    apply_visibility();
}

void companion_pages_show(companion_page_t page)
{
    s_page = page;
    apply_visibility();
}

void companion_pages_cycle(void)
{
    // Toggle Eyes <-> Chat (Clock page removed — the agent handles time).
    s_page = (s_page == COMPANION_PAGE_EYES) ? COMPANION_PAGE_CHAT : COMPANION_PAGE_EYES;
    ESP_LOGI(TAG, "page → %s", s_page == COMPANION_PAGE_EYES ? "eyes" : "chat");
    apply_visibility();
}

companion_page_t companion_pages_current(void) { return s_page; }

void companion_pages_on_voice_status(const char *text)
{
    if (!text || !text[0]) return;
    if (!strncmp(text, "Listening", 9) ||
        !strncmp(text, "Thinking", 8) ||
        !strncmp(text, "Reasoning", 9) ||
        !strncmp(text, "Using ", 6) ||
        !strncmp(text, "Speaking", 8) ||
        !strncmp(text, "Hold ", 5)) {
        companion_pages_show(COMPANION_PAGE_EYES);
    }
}

void companion_pages_on_chat_stream(void)
{
    companion_pages_show(COMPANION_PAGE_CHAT);
}

void companion_pages_tick(void)
{
    // no-op (clock page removed; kept for API compatibility)
}
