// NIMO-style spring-physics face (eyes + mouth). Owned by chat_ui.
#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "chat_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

void face_engine_create(lv_obj_t *scr);
void face_engine_set_mood(chat_ui_face_mood_t mood);
chat_ui_face_mood_t face_engine_get_mood(void);
void face_engine_set_visible(bool visible);
bool face_engine_is_visible(void);
void face_engine_set_active(bool active);   // pause timer when Eyes page is not shown
lv_obj_t *face_engine_root(void);

#ifdef __cplusplus
}
#endif
