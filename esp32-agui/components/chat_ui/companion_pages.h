// Eyes / Clock / Chat page shell for the companion UI.
#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "chat_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COMPANION_PAGE_EYES = 0,
    COMPANION_PAGE_CLOCK,
    COMPANION_PAGE_CHAT,
} companion_page_t;

void companion_pages_create(lv_obj_t *scr);
void companion_pages_cycle(void);
void companion_pages_show(companion_page_t page);
companion_page_t companion_pages_current(void);
void companion_pages_on_voice_status(const char *text);  // Listening/Speaking/… → Eyes
void companion_pages_on_chat_stream(void);               // user/assistant text → Chat
void companion_pages_tick(void);                         // clock refresh (from face/status)

#ifdef __cplusplus
}
#endif
