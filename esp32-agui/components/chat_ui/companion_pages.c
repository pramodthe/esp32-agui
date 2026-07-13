// Companion page overlays: Clock (+ Eyes/Chat orchestration).
// Weather lives on the agent via web_search — no on-device OWM page.
#include "companion_pages.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "device_tools.h"
#include "face_engine.h"

static const char *TAG = "companion";

#define SAFE_INSET 22

static companion_page_t s_page = COMPANION_PAGE_EYES;
static lv_obj_t *s_clock;
static lv_obj_t *s_clock_time;
static lv_obj_t *s_clock_date;
static lv_obj_t *s_clock_batt;
static lv_timer_t *s_page_timer;

static void set_hidden(lv_obj_t *o, bool hide)
{
    if (!o) return;
    if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_page(lv_obj_t *scr)
{
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(p, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static void clock_refresh(void)
{
    if (!s_clock_time) return;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char tbuf[32], dbuf[48];
    strftime(tbuf, sizeof tbuf, "%I:%M:%S %p", &lt);
    // Strip leading zero from hour for a cleaner look
    if (tbuf[0] == '0') memmove(tbuf, tbuf + 1, strlen(tbuf));
    strftime(dbuf, sizeof dbuf, "%a %b %d", &lt);
    lv_label_set_text(s_clock_time, (now < 1700000000) ? "--:--:--" : tbuf);
    lv_label_set_text(s_clock_date, (now < 1700000000) ? "Syncing time..." : dbuf);

    int pct = -1;
    bool plugged = false;
    char status[40] = {0};
    if (device_tools_battery_read(&pct, &plugged, status, sizeof status) && pct >= 0) {
        char b[64];
        snprintf(b, sizeof b, "%d%%  %s", pct, plugged ? "USB" : "Batt");
        lv_label_set_text(s_clock_batt, b);
    } else {
        lv_label_set_text(s_clock_batt, "");
    }
}

static void page_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_page == COMPANION_PAGE_CLOCK) clock_refresh();
}

static void apply_visibility(void)
{
    bool eyes = (s_page == COMPANION_PAGE_EYES);
    bool clock = (s_page == COMPANION_PAGE_CLOCK);
    // Chat = none of the overlays

    face_engine_set_visible(eyes);
    face_engine_set_active(eyes);
    set_hidden(s_clock, !clock);

    if (clock) clock_refresh();
}

void companion_pages_create(lv_obj_t *scr)
{
    s_clock = make_page(scr);
    s_clock_time = lv_label_create(s_clock);
    lv_obj_set_style_text_color(s_clock_time, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_clock_time, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_clock_time, "--:--:--");
    lv_obj_align(s_clock_time, LV_ALIGN_CENTER, 0, -30);

    s_clock_date = lv_label_create(s_clock);
    lv_obj_set_style_text_color(s_clock_date, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_text_font(s_clock_date, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_clock_date, "");
    lv_obj_align(s_clock_date, LV_ALIGN_CENTER, 0, 30);

    s_clock_batt = lv_label_create(s_clock);
    lv_obj_set_style_text_color(s_clock_batt, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_text_font(s_clock_batt, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_clock_batt, "");
    lv_obj_align(s_clock_batt, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET - 8);

    s_page_timer = lv_timer_create(page_timer_cb, 500, NULL);
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
    s_page = (companion_page_t)((s_page + 1) % 3);
    ESP_LOGI(TAG, "page → %d", (int)s_page);
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
    // no-op; lv_timer handles refresh
}
