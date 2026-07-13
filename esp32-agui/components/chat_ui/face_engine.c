// NIMO-faithful eyes/mouth for the AMOLED companion.
//
// Ported from the reference NIMO Arduino sketch (SH1106 128x64 mono OLED): the same
// rounded-rect eyes, rounded-rect pupils + white glint, overlay-masked expressions
// (happy/sleepy/sad/think), angry eyes with diagonal brows + "!" marks + teeth, dizzy
// circle-eyes with two orbiting pupils + star bitmaps, and NIMO's pixel-parabola mouths
// with heart/zzz particles.
//
// NIMO draws pixels straight into a framebuffer, so we reproduce that exactly: a small
// software GFX (rounded rect / rect / circle / line / bitmap) writes white/black into a
// PSRAM canvas, and every NIMO coordinate is scaled ~2.9x (128 -> 368) at draw time. The
// canvas is one lv_canvas invalidated each tick. Physics (spring eyes) and mood triggers
// (tilt -> suspicious, shake -> dizzy -> angry) are unchanged.
#include "face_engine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "imu_qmi8658.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"

static const char *TAG = "face_engine";

#define FACE_TIMER_MS   70
#define NIMO_W          128     // reference screen the coordinates are authored in
#define NIMO_H          64
#define SHAKE_THRESH    0.55f

#define WHITE 0xFFFFu
#define BLACK 0x0000u

// Internal NIMO-style moods (drawing vocabulary). Voice states reuse the NORMAL path
// with small tweaks (bigger eyes / look-up lid / talking mouth).
enum {
    NM_NORMAL = 0, NM_LISTEN, NM_THINK, NM_SPEAK,
    NM_HAPPY, NM_SLEEPY, NM_SAD, NM_ANGRY, NM_SURPRISED, NM_DIZZY, NM_LOVE, NM_SUSPICIOUS,
};

// ---- 1bpp bitmaps copied verbatim from the NIMO sketch (16x16, MSB-first) ----
static const uint8_t bmp_heart[] = {
  0x00,0x00,0x0c,0x60,0x1e,0xf0,0x3f,0xf8,0x7f,0xfc,0x7f,0xfc,0x7f,0xfc,0x3f,0xf8,
  0x1f,0xf0,0x0f,0xe0,0x07,0xc0,0x03,0x80,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
static const uint8_t bmp_zzz[] = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x00,0x0c,0x00,0x18,0x00,0x30,0x00,0x7e,
  0x00,0x00,0x3c,0x00,0x0c,0x00,0x18,0x00,0x30,0x00,0x7c,0x00,0x00,0x00,0x00,0x00
};
static const uint8_t bmp_dizzy_stars[] = {
  0x08,0x20,0x14,0x50,0x22,0x88,0x41,0x04,0x82,0x02,0x41,0x04,0x22,0x88,0x14,0x50,
  0x08,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
static const uint8_t bmp_angry_mark[] = {
  0x00,0x00,0x00,0x00,0x08,0x00,0x1c,0x00,0x3e,0x00,0x7f,0x00,0x3e,0x00,0x1c,0x00,
  0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

// ---- eye physics (NIMO coordinate space) ----
typedef struct {
    float x, y, w, h;       // current
    float tx, ty, tw, th;   // target
    float vx, vy, vw, vh;   // velocity
    float px, py;           // pupil offset (current)
    float tpx, tpy;         // pupil offset (target)
    float pvx, pvy;
} eye_t;

static lv_obj_t  *s_root;
static lv_obj_t  *s_canvas;
static uint16_t  *s_fb;             // W*H canvas (white/black + red for angry)
static int        s_W, s_H;
static float      s_scale;
static uint16_t   s_red;            // RGB565 red as stored in the canvas (swap-aware)
static lv_timer_t *s_timer;

static chat_ui_face_mood_t s_mood = CHAT_UI_FACE_IDLE;
static chat_ui_face_mood_t s_override = CHAT_UI_FACE_IDLE;   // dizzy/angry/suspicious from motion
static int64_t   s_override_until_ms;
static int64_t   s_shake_start_ms;
static bool      s_active = true;
static bool      s_blinking;
static uint32_t  s_blink_last_ms, s_next_blink_ms;
static int       s_speak_phase;
static eye_t     s_L, s_R;

static uint32_t face_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ================= software GFX over the PSRAM canvas (real/buffer pixels) =============
#define SC(v)  ((int)lroundf((v) * s_scale))

static inline void rpx(int x, int y, uint16_t c)
{
    if ((unsigned)x < (unsigned)s_W && (unsigned)y < (unsigned)s_H) s_fb[y * s_W + x] = c;
}

static void rfill(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_W) w = s_W - x;
    if (y + h > s_H) h = s_H - y;
    for (int j = 0; j < h; j++) {
        uint16_t *p = &s_fb[(y + j) * s_W + x];
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

static void rfill_circle(int cx, int cy, int r, uint16_t c)
{
    if (r < 0) return;
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)lround(sqrt((double)r * r - (double)dy * dy));
        rfill(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

static void rdraw_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        rpx(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void rfill_round_rect(int x, int y, int w, int h, int r, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) { rfill(x, y, w, h, c); return; }
    for (int j = 0; j < h; j++) {
        int inset = 0;
        if (j < r) {
            int k = r - 1 - j;
            inset = r - (int)floor(sqrt((double)r * r - (double)k * k));
        } else if (j >= h - r) {
            int k = r - 1 - (h - 1 - j);
            inset = r - (int)floor(sqrt((double)r * r - (double)k * k));
        }
        rfill(x + inset, y + j, w - 2 * inset, 1, c);
    }
}

// NIMO-coordinate wrappers (scaled to the panel)
static void gfx_rrect(float x, float y, float w, float h, float r, uint16_t c)
{ rfill_round_rect(SC(x), SC(y), SC(w), SC(h), SC(r), c); }
static void gfx_rect(float x, float y, float w, float h, uint16_t c)
{ rfill(SC(x), SC(y), SC(w), SC(h), c); }
static void gfx_circle(float cx, float cy, float r, uint16_t c)
{ rfill_circle(SC(cx), SC(cy), SC(r), c); }
static void gfx_line(float x0, float y0, float x1, float y1, uint16_t c)
{ rdraw_line(SC(x0), SC(y0), SC(x1), SC(y1), c); }
static void gfx_px(float x, float y, uint16_t c)   // one NIMO pixel = one scaled block
{ int b = (int)ceilf(s_scale); rfill(SC(x), SC(y), b, b, c); }

static void gfx_bitmap(float x, float y, const uint8_t *bmp, int bw, int bh, uint16_t c)
{
    int bytes_per_row = (bw + 7) / 8;
    int b = (int)ceilf(s_scale);
    int ox = SC(x), oy = SC(y);
    for (int j = 0; j < bh; j++)
        for (int i = 0; i < bw; i++)
            if (bmp[j * bytes_per_row + i / 8] & (0x80 >> (i & 7)))
                rfill(ox + (int)lroundf(i * s_scale), oy + (int)lroundf(j * s_scale), b, b, c);
}

// Solid slanted brow/lid band across the top of an eye (used for sad + angry, which NIMO
// draws as a stack of diagonal lines — filled here so it stays solid when scaled up).
static void gfx_slant_top(float ix, float iy, float iw, float drop, float thick, int down)
{
    int x0 = SC(ix), x1 = SC(ix + iw);
    if (x1 <= x0) return;
    for (int x = x0; x <= x1; x++) {
        float f = (float)(x - x0) / (float)(x1 - x0);
        float yy = iy + (down ? f * drop : (1.0f - f) * drop);
        rfill(x, SC(yy), 1, SC(thick), BLACK);
    }
}

// ================================= NIMO draw functions ================================
static void draw_normal_eye(const eye_t *e, bool is_left, int nm)
{
    float ix = e->x, iy = e->y, iw = e->w, ih = e->h;
    float r = (iw < 20) ? 3 : 8;
    gfx_rrect(ix, iy, iw, ih, r, WHITE);

    float cx = ix + iw / 2, cy = iy + ih / 2;
    float pw = iw / 2.2f, ph = ih / 2.2f;
    float pxp = cx + e->px - pw / 2, pyp = cy + e->py - ph / 2;
    if (pxp < ix) pxp = ix;
    if (pxp + pw > ix + iw) pxp = ix + iw - pw;
    if (pyp < iy) pyp = iy;
    if (pyp + ph > iy + ih) pyp = iy + ih - ph;
    gfx_rrect(pxp, pyp, pw, ph, r / 2.0f, BLACK);
    if (iw > 15 && ih > 15) gfx_circle(pxp + pw - 4, pyp + 4, 2, WHITE);   // glint

    if (nm == NM_HAPPY || nm == NM_LOVE)      gfx_rect(ix, iy + ih - 10, iw, 12, BLACK);
    else if (nm == NM_SLEEPY)                 gfx_rect(ix, iy, iw, ih / 2, BLACK);
    else if (nm == NM_THINK)                  gfx_rect(ix, iy, iw, ih / 3, BLACK);
    else if (nm == NM_SUSPICIOUS && !is_left) gfx_rect(ix, iy, iw, ih / 3, BLACK);  // skeptic brow
    else if (nm == NM_SAD)                    gfx_slant_top(ix, iy, iw, 4, 8, is_left ? 1 : 0);
}

static void draw_angry_eye(const eye_t *e, bool is_left)
{
    float ix = e->x, iy = e->y, iw = e->w, ih = e->h;
    gfx_rrect(ix, iy, iw, ih, 8, s_red);   // angry -> red eyes

    float cx = ix + iw / 2, cy = iy + ih / 2;
    float ps = iw / 2.5f;
    gfx_rrect(cx - 2 - ps / 2, cy - ps / 2, ps, ps, ps / 2, BLACK);

    gfx_slant_top(ix - 2, iy - 2, iw + 4, 8, 8, is_left ? 1 : 0);        // frown brow
    gfx_bitmap(is_left ? ix - 12 : ix + iw - 4, iy - 6, bmp_angry_mark, 16, 16, WHITE);
    for (int i = 0; i < 3; i++)                                          // gritted teeth
        gfx_line(ix + 2 + i * 4, iy + ih - 2, ix + 6 + i * 4, iy + ih - 6, WHITE);
}

static void draw_dizzy_eye(const eye_t *e)
{
    float ix = e->x, iy = e->y, iw = e->w;
    float cx = ix + iw / 2, cy = iy + e->h / 2;
    gfx_circle(cx, cy, iw / 2, WHITE);

    float ang = (face_now_ms() - (uint32_t)s_shake_start_ms) * 0.025f;
    float rad = iw / 3;
    gfx_circle(cx + cosf(ang) * rad, cy + sinf(ang) * rad, 4, BLACK);
    gfx_circle(cx + cosf(ang) * rad + 2, cy + sinf(ang) * rad - 2, 1, WHITE);
    gfx_circle(cx - cosf(ang) * rad, cy - sinf(ang) * rad, 3, BLACK);
}

static void draw_mouth(int nm)
{
    const float mx = 64, my = 55;

    if (nm == NM_DIZZY) {                                  // woozy wave
        for (int i = -9; i <= 9; i++)
            gfx_px(mx + i, my + sinf(i * 0.7f + face_now_ms() * 0.03f) * 4, WHITE);
        return;
    }
    if (nm == NM_ANGRY) {                                  // gritted teeth
        gfx_rect(mx - 9, my - 3, 18, 6, BLACK);
        for (int i = -6; i <= 6; i += 3) gfx_line(mx + i, my - 3, mx + i, my + 3, WHITE);
        return;
    }
    switch (nm) {
    case NM_HAPPY:                                         // bold smile
        for (int i = -10; i <= 10; i++) {
            float y = my - (i * i / 22.0f);
            gfx_px(mx + i, y, WHITE);
            gfx_px(mx + i, y - 1, WHITE);
        }
        break;
    case NM_SAD:                                           // frown
        for (int i = -9; i <= 9; i++) {
            float y = my + (i * i / 26.0f);
            gfx_px(mx + i, y, WHITE);
            gfx_px(mx + i, y + 1, WHITE);
        }
        break;
    case NM_SURPRISED:                                     // open "O"
        gfx_circle(mx, my + 1, 6, WHITE);
        gfx_circle(mx, my + 1, 4, BLACK);
        break;
    case NM_LISTEN:                                        // small attentive "o"
        gfx_circle(mx, my, 4, WHITE);
        gfx_circle(mx, my, 2, BLACK);
        break;
    case NM_LOVE:                                          // heart mouth
        gfx_bitmap(mx - 8, my - 4, bmp_heart, 16, 16, WHITE);
        break;
    case NM_SPEAK: {                                       // talking: open/close cavity
        float h = 4 + (s_speak_phase % 3) * 6.0f;          // 4 -> 10 -> 16
        gfx_rrect(mx - 9, my - h / 2, 18, h, 4, WHITE);
        float ih = h - 5;
        if (ih >= 1) gfx_rrect(mx - 6, my - ih / 2, 12, ih, 3, BLACK);
        break;
    }
    default:                                               // NORMAL / THINK / SLEEPY / SUSPICIOUS
        for (int i = -6; i <= 6; i++) gfx_px(mx + i, my - (i * i / 45.0f), WHITE);
        break;
    }
}

static int nimo_mood(chat_ui_face_mood_t m)
{
    switch (m) {
    case CHAT_UI_FACE_LISTEN:     return NM_LISTEN;
    case CHAT_UI_FACE_THINK:      return NM_THINK;
    case CHAT_UI_FACE_SPEAK:      return NM_SPEAK;
    case CHAT_UI_FACE_HAPPY:      return NM_HAPPY;
    case CHAT_UI_FACE_SLEEPY:     return NM_SLEEPY;
    case CHAT_UI_FACE_SAD:        return NM_SAD;
    case CHAT_UI_FACE_ANGRY:      return NM_ANGRY;
    case CHAT_UI_FACE_SURPRISED:  return NM_SURPRISED;
    case CHAT_UI_FACE_DIZZY:      return NM_DIZZY;
    case CHAT_UI_FACE_LOVE:       return NM_LOVE;
    case CHAT_UI_FACE_SUSPICIOUS: return NM_SUSPICIOUS;
    default:                      return NM_NORMAL;
    }
}

static chat_ui_face_mood_t effective_mood(void)
{
    if (s_override != CHAT_UI_FACE_IDLE && (int64_t)face_now_ms() < s_override_until_ms)
        return s_override;
    return s_mood;
}

// ---- physics targets, blink, spring ----
static void apply_targets(int nm, float roll, float pitch)
{
    float lx = 28, ly = 18, rx = 80, ry = 18, ew = 32, eh = 32;
    if (nm == NM_LISTEN) { ew = 36; eh = 36; lx = 26; ly = 16; rx = 78; ry = 16; }   // attentive

    float dx = roll / 15.0f, dy = pitch / 15.0f;
    s_L.tx = lx + dx; s_L.ty = ly + dy; s_L.tw = ew; s_L.th = eh;
    s_R.tx = rx + dx; s_R.ty = ry + dy; s_R.tw = ew; s_R.th = eh;

    float tpx = clampf(roll / 5.0f, -12, 12);
    float tpy = clampf(pitch / 5.0f, -10, 10);
    if (nm == NM_THINK)      tpy -= 6;    // look up
    if (nm == NM_SUSPICIOUS) tpx += 8;    // side-eye
    s_L.tpx = s_R.tpx = tpx;
    s_L.tpy = s_R.tpy = tpy;
}

static void update_blink(int nm)
{
    uint32_t now = face_now_ms();
    bool fast = (nm == NM_DIZZY || nm == NM_ANGRY);
    uint32_t bd   = fast ? 60 : 120;
    uint32_t bmin = fast ? 300 : (nm == NM_SUSPICIOUS ? 3500 : 2000);
    uint32_t bmax = fast ? 800 : (nm == NM_SUSPICIOUS ? 7000 : 6000);
    if (now > s_next_blink_ms) {
        s_blinking = true;
        s_blink_last_ms = now;
        s_next_blink_ms = now + bmin + (esp_random() % (bmax - bmin + 1));
    }
    if (s_blinking) {
        s_L.th = s_R.th = 2;   // eyelids down
        if (now - s_blink_last_ms > bd) s_blinking = false;
    }
}

static void eye_spring(eye_t *e)
{
    const float K = 0.15f, D = 0.65f, PK = 0.20f, PD = 0.60f;
    e->vx = (e->vx + (e->tx - e->x) * K) * D; e->x += e->vx;
    e->vy = (e->vy + (e->ty - e->y) * K) * D; e->y += e->vy;
    e->vw = (e->vw + (e->tw - e->w) * K) * D; e->w += e->vw;
    e->vh = (e->vh + (e->th - e->h) * K) * D; e->h += e->vh;
    e->pvx = (e->pvx + (e->tpx - e->px) * PK) * PD; e->px += e->pvx;
    e->pvy = (e->pvy + (e->tpy - e->py) * PK) * PD; e->py += e->pvy;
}

static void draw_scene(int nm)
{
    if (nm == NM_DIZZY) {
        draw_dizzy_eye(&s_L);
        draw_dizzy_eye(&s_R);
        int off = (face_now_ms() / 80) % 4;
        gfx_bitmap(6 - off, 0, bmp_dizzy_stars, 16, 16, WHITE);
        gfx_bitmap(106 + off, 0, bmp_dizzy_stars, 16, 16, WHITE);
    } else if (nm == NM_ANGRY) {
        draw_angry_eye(&s_L, true);
        draw_angry_eye(&s_R, false);
    } else {
        draw_normal_eye(&s_L, true, nm);
        draw_normal_eye(&s_R, false, nm);
        if (nm == NM_LOVE)        gfx_bitmap(56, 0, bmp_heart, 16, 16, WHITE);
        else if (nm == NM_SLEEPY) gfx_bitmap(110, 0, bmp_zzz, 16, 16, WHITE);
    }
    draw_mouth(nm);
}

static void face_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_root || !s_active || !s_fb) return;
    if (lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) return;

    uint32_t now = face_now_ms();

    // Shake -> dizzy (~1.5s) -> angry (~2s)
    float shake = imu_qmi8658_shake_intensity();
    if (shake > SHAKE_THRESH && s_override != CHAT_UI_FACE_DIZZY) {
        s_override = CHAT_UI_FACE_DIZZY;
        s_override_until_ms = (int64_t)now + 1500;
        s_shake_start_ms = now;
    } else if (s_override == CHAT_UI_FACE_DIZZY && (int64_t)now >= s_override_until_ms) {
        s_override = CHAT_UI_FACE_ANGRY;
        s_override_until_ms = (int64_t)now + 2000;
    } else if (s_override == CHAT_UI_FACE_ANGRY && (int64_t)now >= s_override_until_ms) {
        s_override = CHAT_UI_FACE_IDLE;
    }

    float roll = 0, pitch = 0;
    imu_qmi8658_get_tilt(&roll, &pitch);   // drives gaze only (see apply_targets)

    int nm = nimo_mood(effective_mood());
    if (nm == NM_SPEAK) s_speak_phase++;

    apply_targets(nm, roll, pitch);
    update_blink(nm);
    eye_spring(&s_L);
    eye_spring(&s_R);

    memset(s_fb, 0, (size_t)s_W * s_H * sizeof(uint16_t));   // clear to black
    draw_scene(nm);
    lv_obj_invalidate(s_canvas);
}

void face_engine_create(lv_obj_t *scr)
{
    s_root = lv_obj_create(scr);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_scale = (float)BSP_LCD_H_RES / NIMO_W;      // ~2.875
    s_W = BSP_LCD_H_RES;
    s_H = (int)lroundf(NIMO_H * s_scale);
    s_fb = heap_caps_malloc((size_t)s_W * s_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "canvas alloc failed (%d bytes)", s_W * s_H * 2);
        return;
    }
    memset(s_fb, 0, (size_t)s_W * s_H * sizeof(uint16_t));
    s_red = lv_color_hex(0xFF2222).full;   // angry-eye colour (matches the alarm ring)

    s_canvas = lv_canvas_create(s_root);
    lv_canvas_set_buffer(s_canvas, s_fb, s_W, s_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Seed eyes at their NIMO rest pose.
    memset(&s_L, 0, sizeof s_L);
    memset(&s_R, 0, sizeof s_R);
    s_L.x = s_L.tx = 28; s_L.y = s_L.ty = 18; s_L.w = s_L.tw = 32; s_L.h = s_L.th = 32;
    s_R.x = s_R.tx = 80; s_R.y = s_R.ty = 18; s_R.w = s_R.tw = 32; s_R.h = s_R.th = 32;
    s_next_blink_ms = face_now_ms() + 1500;

    draw_scene(NM_NORMAL);
    lv_obj_invalidate(s_canvas);

    s_timer = lv_timer_create(face_tick, FACE_TIMER_MS, NULL);
    ESP_LOGI(TAG, "NIMO face up (%dx%d canvas, scale %.2f)", s_W, s_H, s_scale);
}

void face_engine_set_mood(chat_ui_face_mood_t mood) { s_mood = mood; }
chat_ui_face_mood_t face_engine_get_mood(void) { return s_mood; }

void face_engine_set_visible(bool visible)
{
    if (!s_root) return;
    if (visible) lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

bool face_engine_is_visible(void)
{
    return s_root && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

void face_engine_set_active(bool active)
{
    s_active = active;
    if (s_timer) {
        if (active) lv_timer_resume(s_timer);
        else        lv_timer_pause(s_timer);
    }
}

lv_obj_t *face_engine_root(void) { return s_root; }
