#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/led_strip.h>
#include <stdio.h>
#include <string.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/keymap.h>
#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

/* ---------- settings you may want to change ---------- */
#define INVERT_COLORS 0   /* set to 1 if the screen looks inverted */
#define LED_START_HUE 240 /* 0 red, 120 green, 240 blue */
#define LED_START_BRT 40  /* percent */
#define TICK_MS 250       /* pet speed: lower = faster, uses more power */

#define W 128
#define H 64
#define LED_COUNT 2

/* ---------- 1-bit framebuffer shown as an LVGL image ---------- */
static lv_color_t fb[W * H]; /* 1 byte per pixel at 1-bit color depth */
#define INK_COLOR (INVERT_COLORS ? lv_color_white() : lv_color_black())
#define BG_COLOR (INVERT_COLORS ? lv_color_black() : lv_color_white())
static lv_img_dsc_t fb_dsc = {
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = W,
    .header.h = H,
    .data_size = sizeof(fb),
    .data = (const uint8_t *)fb,
};
static lv_obj_t *img;
static uint32_t frame;
static int64_t popup_until;

static void px(int x, int y, bool on) {
    if (x < 0 || x >= W || y < 0 || y >= H) {
        return;
    }
    fb[y * W + x] = on ? INK_COLOR : BG_COLOR;
}

static void rect(int x, int y, int w, int h, bool on) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            px(x + i, y + j, on);
        }
    }
}

static void art(int x, int y, const char *const *rows, int n, bool on) {
    for (int r = 0; r < n; r++) {
        for (int c = 0; rows[r][c]; c++) {
            if (rows[r][c] == '#') {
                px(x + c, y + r, on);
            }
        }
    }
}


static void art_scaled(int x, int y, const char *const *rows,
                       int n, int scale, bool on) {
    for (int r = 0; r < n; r++) {
        for (int c = 0; rows[r][c]; c++) {
            if (rows[r][c] == '#') {
                rect(x + c * scale, y + r * scale,
                     scale, scale, on);
            }
        }
    }
}


/* ---------- 3x5 font ---------- */
static const uint8_t F_DIG[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
};
static const uint8_t F_ALP[26][5] = {
    {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {3, 4, 4, 4, 3}, {6, 5, 5, 5, 6}, {7, 4, 6, 4, 7},
    {7, 4, 6, 4, 4}, {3, 4, 5, 5, 3}, {5, 5, 7, 5, 5}, {7, 2, 2, 2, 7}, {1, 1, 1, 5, 2},
    {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7}, {5, 7, 7, 5, 5}, {6, 5, 5, 5, 5}, {2, 5, 5, 5, 2},
    {6, 5, 6, 4, 4}, {2, 5, 5, 6, 3}, {6, 5, 6, 5, 5}, {3, 4, 2, 1, 6}, {7, 2, 2, 2, 2},
    {5, 5, 5, 5, 7}, {5, 5, 5, 5, 2}, {5, 5, 7, 7, 5}, {5, 5, 2, 5, 5}, {5, 5, 2, 2, 2},
    {7, 1, 2, 4, 7},
};
static const uint8_t F_PCT[5] = {5, 1, 2, 4, 5};
static const uint8_t F_BLANK[5] = {0, 0, 0, 0, 0};

static const uint8_t *glyph(char c) {
    if (c >= '0' && c <= '9') {
        return F_DIG[c - '0'];
    }
    if (c >= 'A' && c <= 'Z') {
        return F_ALP[c - 'A'];
    }
    if (c == '%') {
        return F_PCT;
    }
    return F_BLANK;
}

static void text(int x, int y, const char *s, int sc, bool on) {
    for (; *s; s++) {
        const uint8_t *g = glyph(*s);
        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 3; c++) {
                if ((g[r] >> (2 - c)) & 1) {
                    rect(x + c * sc, y + r * sc, sc, sc, on);
                }
            }
        }
        x += 4 * sc;
    }
}

static int text_w(const char *s, int sc) {
    int n = strlen(s);
    return n ? n * 4 * sc - sc : 0;
}

/* ---------- LED engine (2 LEDs on the chosen underglow strip) ---------- */
static const struct device *strip = DEVICE_DT_GET(DT_CHOSEN(zmk_underglow));
enum { LM_SOLID, LM_BREATHE, LM_RAINBOW, LM_SWIRL, LM_COUNT };
static const char *const LM_NAME[LM_COUNT] = {"SOLID", "BREATHE", "RAINBOW", "SWIRL"};
static struct {
    bool on;
    uint8_t mode;
    uint16_t hue;
    uint8_t bri;
} led = {true, LM_SOLID, LED_START_HUE, LED_START_BRT};

static struct k_work_delayable led_work;
static struct k_work_delayable tick_work;

static void hsv(uint16_t h, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
    h %= 360;
    uint8_t region = h / 60;
    uint8_t rem = (h % 60) * 255 / 60;
    uint8_t q = v * (255 - rem) / 255;
    uint8_t t = v * rem / 255;
    switch (region) {
    case 0: *r = v; *g = t; *b = 0; break;
    case 1: *r = q; *g = v; *b = 0; break;
    case 2: *r = 0; *g = v; *b = t; break;
    case 3: *r = 0; *g = q; *b = v; break;
    case 4: *r = t; *g = 0; *b = v; break;
    default: *r = v; *g = 0; *b = q; break;
    }
}

static void led_work_fn(struct k_work *work) {
    struct led_rgb pix[LED_COUNT];
    memset(pix, 0, sizeof(pix));
    bool active = zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE;
    bool animated = led.on && active && led.mode != LM_SOLID;

    if (led.on && active) {
        uint32_t t = k_uptime_get_32();
        uint8_t v = (uint32_t)led.bri * 255 / 100;
        for (int i = 0; i < LED_COUNT; i++) {
            uint16_t h = led.hue;
            uint8_t vv = v;
            if (led.mode == LM_BREATHE) {
                uint32_t ph = t % 3000;
                uint32_t tri = ph < 1500 ? ph : 3000 - ph;
                vv = (uint32_t)v * (25 + tri * 230 / 1500) / 255;
            } else if (led.mode == LM_RAINBOW) {
                h = (t / 30) % 360;
            } else if (led.mode == LM_SWIRL) {
                h = (t / 30 + i * 180) % 360;
            }
            hsv(h, vv, &pix[i].r, &pix[i].g, &pix[i].b);
        }
    }
    if (device_is_ready(strip)) {
        led_strip_update_rgb(strip, pix, LED_COUNT);
    }
    k_work_reschedule(&led_work, K_MSEC(animated ? 40 : 500));
}

static void led_key(int pos) {
    switch (pos) {
    case 14: led.on = !led.on; break;
    case 15: led.bri = led.bri >= 100 ? 100 : led.bri + 10; break;
    case 16: led.bri = led.bri >= 10 ? led.bri - 10 : 0; break;
    case 19: led.mode = (led.mode + 1) % LM_COUNT; led.on = true; break;
    case 20: led.hue = (led.hue + 15) % 360; break;
    case 21: led.hue = (led.hue + 345) % 360; break;
    default: return;
    }
    popup_until = k_uptime_get() + 2000;
    k_work_reschedule(&led_work, K_NO_WAIT);
    k_work_reschedule_for_queue(zmk_display_work_q(), &tick_work, K_NO_WAIT);
}

static int wowpad_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev != NULL && ev->state && zmk_keymap_layer_active(1)) {
        led_key(ev->position);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(wowpad, wowpad_listener);
ZMK_SUBSCRIPTION(wowpad, zmk_position_state_changed);

/* ---------- pet ---------- */
static const char *const PET_A[4] = {".##.", "#..#", "####", "#..#"};
static const char *const PET_B[4] = {".##.", "#..#", "####", ".##."};
static const char *const ART_POOP[2] = {".#.", "###"};
static const char *const ART_FOOD[3] = {".#.", "###", "###"};

#define X_MAX 124
#define Y_MIN 12
#define Y_MAX 58

enum pet_state { P_WANDER, P_PAUSE, P_PLAY, P_FOOD, P_EAT, P_POOP };
static struct {
    int x, y, tx, ty, timer;
    enum pet_state st;
    bool food;
    int fx, fy;
    int64_t next_food, poop_due;
} pet = {.x = 60, .y = 40, .tx = 60, .ty = 40, .st = P_PAUSE, .timer = 8};

static struct {
    bool on;
    int x, y;
    int64_t born;
} poops[3];

static uint32_t rs = 2463534242u;
static uint32_t rnd(uint32_t n) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs % n;
}

static bool step_to(int tx, int ty) {
    int dx = tx - pet.x, dy = ty - pet.y;
    dx = dx > 2 ? 2 : (dx < -2 ? -2 : dx);
    dy = dy > 2 ? 2 : (dy < -2 ? -2 : dy);
    pet.x += dx;
    pet.y += dy;
    return pet.x == tx && pet.y == ty;
}

static void poop_spawn(int x, int y, int64_t now) {
    int slot = 0;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < 3; i++) {
        if (!poops[i].on) {
            slot = i;
            break;
        }
        if (poops[i].born < oldest) {
            oldest = poops[i].born;
            slot = i;
        }
    }
    poops[slot].on = true;
    poops[slot].x = x;
    poops[slot].y = y;
    poops[slot].born = now;
}

static void pet_tick(void) {
    int64_t now = k_uptime_get();
    frame++;

    if (!pet.food && now >= pet.next_food) {
        pet.food = true;
        pet.fx = 2 + rnd(120);
        pet.fy = Y_MIN + rnd(44);
        pet.next_food = now + 40000 + rnd(40000);
    }
    for (int i = 0; i < 3; i++) {
        if (poops[i].on && now - poops[i].born > 90000) {
            poops[i].on = false;
        }
    }

    switch (pet.st) {
    case P_WANDER:
        if (step_to(pet.tx, pet.ty)) {
            if (rnd(3) == 0) {
                pet.st = P_PLAY;
                pet.timer = 12;
            } else {
                pet.st = P_PAUSE;
                pet.timer = 6 + rnd(8);
            }
        }
        break;
    case P_PAUSE:
        if (--pet.timer <= 0) {
            pet.tx = rnd(X_MAX + 1);
            pet.ty = Y_MIN + rnd(Y_MAX - Y_MIN + 1);
            pet.st = P_WANDER;
        }
        break;
    case P_PLAY:
        if (--pet.timer <= 0) {
            pet.st = P_PAUSE;
            pet.timer = 4;
        }
        break;
    case P_FOOD:
        if (!pet.food) {
            pet.st = P_PAUSE;
            pet.timer = 2;
        } else if (step_to(pet.fx, pet.fy)) {
            pet.st = P_EAT;
            pet.timer = 16;
        }
        break;
    case P_EAT:
        if (--pet.timer <= 0) {
            pet.food = false;
            pet.poop_due = now + 20000 + rnd(25000);
            pet.st = P_PAUSE;
            pet.timer = 4;
        }
        break;
    case P_POOP:
        if (--pet.timer <= 0) {
            pet.st = P_PAUSE;
            pet.timer = 6;
        }
        break;
    }

    if (pet.st == P_WANDER || pet.st == P_PAUSE) {
        if (pet.poop_due && now >= pet.poop_due) {
            pet.poop_due = 0;
            poop_spawn(pet.x + 1, pet.y + 2, now);
            pet.st = P_POOP;
            pet.timer = 6;
        } else if (pet.food) {
            pet.st = P_FOOD;
        }
    }
}

static void draw_pet(void) {
    for (int i = 0; i < 3; i++) {
        if (poops[i].on) {
            art_scaled(poops[i].x, poops[i].y, ART_POOP, 2, 2, true);
        }
    }
    if (pet.food) {
        art_scaled(pet.fx, pet.fy, ART_FOOD, 3, 2, true);
    }
    const char *const *spr = (frame & 1) ? PET_A : PET_B;
    int yy = pet.y;
    if (pet.st == P_PAUSE || pet.st == P_POOP) {
        spr = PET_B;
    }
    if (pet.st == P_PLAY) {
        static const int hop[4] = {0, -3, -5, -3};
        yy += hop[frame & 3];
        px(pet.x - 3, yy + ((frame & 1) ? 0 : 2), true);
        px(pet.x + 6, yy + ((frame & 1) ? 2 : 0), true);
    }
    art_scaled(pet.x, yy, spr, 4, 2, true);
}

/* ---------- top bar (6 px tall) and popup ---------- */
static const char *const ART_BT[6] = {"..#..", "..##.", "#.#.#", ".###.", "..##.", "..#.."};


static void draw_battery(int x, uint8_t soc) {
    for (int i = 0; i < 10; i++) {
        rect(x + i * 2, 0, 2, 2, true);
        rect(x + i * 2, 10, 2, 2, true);
    }

    for (int j = 0; j < 6; j++) {
        rect(x, j * 2, 2, 2, true);
        rect(x + 18, j * 2, 2, 2, true);
    }

    rect(x + 20, 4, 2, 4, true);

    int fill = (soc * 8 + 50) / 100;
    if (soc > 0 && fill == 0) {
        fill = 1;
    }

    rect(x + 2, 2, fill * 2, 8, true);
}



static void draw_top(void) {
    if (zmk_ble_active_profile_is_connected() || ((frame >> 1) & 1)) {
        art_scaled(0, 0, ART_BT, 6, 2, true);
    }

    char d[2] = {'1' + zmk_ble_active_profile_index(), 0};
    text(12, 0, d, 1, true);

    if (zmk_keymap_layer_active(1)) {
        rect(58, 0, 9, 6, true);
        text(59, 0, "FN", 1, false);
    }

    uint8_t soc = zmk_battery_state_of_charge();
    char b[8];
    snprintf(b, sizeof(b), "%u%%", soc);

    draw_battery(105, soc);
    text(103 - text_w(b, 1), 0, b, 1, true);
}


static void draw_popup(void) {
    char l2[24];
    const char *name = led.on ? LM_NAME[led.mode] : "LED OFF";
    l2[0] = 0;
    if (led.on) {
        if (led.mode == LM_SOLID || led.mode == LM_BREATHE) {
            snprintf(l2, sizeof(l2), "HUE %d  BRT %d%%", led.hue, led.bri);
        } else {
            snprintf(l2, sizeof(l2), "BRT %d%%", led.bri);
        }
    }
    rect(16, 14, 96, 30, false);
    for (int x = 16; x < 112; x++) {
        px(x, 14, true);
        px(x, 43, true);
    }
    for (int y = 14; y < 44; y++) {
        px(16, y, true);
        px(111, y, true);
    }
    text((W - text_w(name, 2)) / 2, 18, name, 2, true);
    text((W - text_w(l2, 1)) / 2, 34, l2, 1, true);
}

static void render(void) {
    for (int i = 0; i < W * H; i++) {
        fb[i] = BG_COLOR;
    }
    draw_pet();
    if (k_uptime_get() < popup_until) {
        draw_popup();
    }
    draw_top();
}

static void tick_cb(struct k_work *work) {
    if (img == NULL || zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE) {
        k_work_reschedule_for_queue(zmk_display_work_q(), &tick_work, K_MSEC(1000));
        return;
    }

    pet_tick();
    render();
    lv_obj_invalidate(img);
    k_work_reschedule_for_queue(zmk_display_work_q(), &tick_work, K_MSEC(TICK_MS));
}

#if defined(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)
lv_obj_t *zmk_display_status_screen(void) {
    lv_color_t bg = BG_COLOR;

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, bg, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    img = lv_img_create(screen);
    lv_img_set_src(img, &fb_dsc);
    lv_obj_align(img, LV_ALIGN_TOP_LEFT, 0, 0);

    rs ^= k_uptime_get_32();
    pet.next_food = k_uptime_get() + 20000;
    render();
    k_work_reschedule_for_queue(zmk_display_work_q(), &tick_work, K_MSEC(TICK_MS));
    return screen;
}
#endif /* CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM */

static int wowpad_init() {
    k_work_init_delayable(&led_work, led_work_fn);
    k_work_init_delayable(&tick_work, tick_cb);
    k_work_reschedule(&led_work, K_SECONDS(1));
    return 0;
}
SYS_INIT(wowpad_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
