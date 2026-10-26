// The display task: owns the scene, the frame buffer and everything on the
// internal I2C bus. The MTP handlers only post events (screen_post); keys,
// tilt and battery are polled here between frames.
#include <math.h>
#include <string.h>

#include "board.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hw.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "screen.h"
#include "screen/babel_screen.h"

#define TAG "app"

#define FRAME_MS 33
#define SPLIT_Y 70                  // shaft rows [0, SPLIT_Y) render on core 0
#define LOOKUP_MAX 120
#define LOOKUP_IDLE_US (60 * 1000000LL)
#define HOST_BOOK_US (8 * 1000000LL)

// IMU axes as mounted on the board: which accelerometer axis tilts the view
// sideways and which tilts it forward, and their signs. Unverified on hardware.
#define IMU_AXIS_SIDE 0
#define IMU_AXIS_FWD 1
#define IMU_SIGN_SIDE (-1.0f)
#define IMU_SIGN_FWD (1.0f)

typedef struct { uint8_t type; uint32_t a, b; } screen_ev_t;

static QueueHandle_t evq;
static bs_scene_t scene;
static uint8_t fb[BS_W * BS_H];

static struct { int mode; int brightness; } cfg = { BS_MODE_B70, 160 };

static uint16_t host_path[BS_PATH_MAX];
static int host_len;
static int64_t host_book_until;

static uint8_t lookup[LOOKUP_MAX];
static int lookup_len;
static int64_t last_key_us;

static bool imu_ok;
static SemaphoreHandle_t helper_go, helper_done;
static bs_params_t frame_params;

/* ------------------------------------------------------------- settings */
void app_load_settings(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    nvs_handle_t h;
    if (err == ESP_OK && nvs_open("babel", NVS_READONLY, &h) == ESP_OK) {
        int8_t mode = cfg.mode;
        uint8_t bri = (uint8_t)cfg.brightness;
        nvs_get_i8(h, "mode", &mode);
        nvs_get_u8(h, "bright", &bri);
        nvs_close(h);
        cfg.mode = mode == BS_MODE_HEX ? BS_MODE_HEX : BS_MODE_B70;
        cfg.brightness = bri < 16 ? 16 : bri;
    }
    babel_mode = cfg.mode;
}

static void save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("babel", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i8(h, "mode", (int8_t)cfg.mode);
    nvs_set_u8(h, "bright", (uint8_t)cfg.brightness);
    nvs_commit(h);
    nvs_close(h);
}

/* --------------------------------------------------------------- events */
void screen_post(screen_ev_type_t type, uint32_t a, uint32_t b)
{
    if (!evq) return;
    screen_ev_t ev = { (uint8_t)type, a, b };
    xQueueSend(evq, &ev, 0);
}

static void ensure_mtp_host(void)
{
    if (scene.host != BS_HOST_MTP) bs_connect(&scene, BS_HOST_MTP);
}

static void follow_host(int depth)
{
    // A host that floods us (prefetching listings) gets the short way there.
    bool replace = bs_queue_pending(&scene) > 6;
    bs_navigate_to(&scene, host_path, depth, 0, replace);
}

static void handle_event(const screen_ev_t *ev)
{
    switch (ev->type) {
        case SCREEN_EV_CONNECT:
            lookup_len = 0;
            host_len = 0;
            if (scene.host != BS_HOST_MTP) bs_connect(&scene, BS_HOST_MTP);
            break;
        case SCREEN_EV_DISCONNECT:
            host_len = 0;
            if (scene.host == BS_HOST_MTP) bs_disconnect(&scene);
            break;
        case SCREEN_EV_ROOT:
            lookup_len = 0;
            host_len = 0;
            ensure_mtp_host();
            follow_host(0);
            break;
        case SCREEN_EV_ENTER: {
            lookup_len = 0;
            int d = (int)ev->a;
            if (d > host_len) d = host_len;
            if (d >= BS_PATH_MAX) break;
            host_path[d] = (uint16_t)ev->b;
            host_len = d + 1;
            ensure_mtp_host();
            follow_host(host_len);
            break;
        }
        case SCREEN_EV_OPEN: {
            lookup_len = 0;
            int k = (int)ev->a < host_len ? (int)ev->a : host_len;
            ensure_mtp_host();
            follow_host(k);
            if (!scene.book.is_open) bs_open(&scene);
            host_book_until = esp_timer_get_time() + HOST_BOOK_US;
            break;
        }
    }
}

/* ----------------------------------------------------------------- keys */
static void relook(void)
{
    if (!scene.host) bs_connect(&scene, BS_HOST_LOOKUP);
    bs_goto_bytes(&scene, lookup, lookup_len, 0, true);
}

static void toast_setting(const char *text)
{
    bs_toast(&scene, text, BS_LAMP, 1.4f);
}

static void handle_key(const key_event_t *k)
{
    last_key_us = esp_timer_get_time();
    if (scene.splash_t0 >= 0) { scene.splash_t0 = -1; return; }   // any key skips the splash
    switch (k->code) {
        case KEY_ENTER:
        case KEY_BTN_A:
            host_book_until = 0;
            bs_toggle_book(&scene);
            break;
        case KEY_BKSP:
        case KEY_DEL:
            if (lookup_len) { lookup_len--; relook(); }
            else bs_up(&scene, 0);
            break;
        case KEY_TAB:
        case KEY_ESC:
            lookup_len = 0;
            if (!scene.host) bs_connect(&scene, BS_HOST_LOOKUP);
            bs_root(&scene);
            break;
        case KEY_UP:
        case KEY_DOWN:
            cfg.brightness += k->code == KEY_UP ? 32 : -32;
            cfg.brightness = cfg.brightness < 16 ? 16 : cfg.brightness > 255 ? 255 : cfg.brightness;
            display_set_brightness((uint8_t)cfg.brightness);
            save_settings();
            break;
        case KEY_OPT:
            if (babel_session_open) { toast_setting("UNPLUG TO CHANGE NAMES"); break; }
            cfg.mode = cfg.mode == BS_MODE_HEX ? BS_MODE_B70 : BS_MODE_HEX;
            babel_mode = cfg.mode;
            bs_set_mode(&scene, cfg.mode);
            lookup_len = 0;
            toast_setting(cfg.mode == BS_MODE_HEX ? "NAMES: HEX 00-FF" : "NAMES: BABEL-70");
            save_settings();
            break;
        case KEY_ALT:
            scene.show_fps = !scene.show_fps;
            break;
        default:
            if (k->code >= 32 && k->code < 127 && lookup_len < LOOKUP_MAX) {
                lookup[lookup_len++] = k->code;
                relook();
            }
            break;
    }
}

/* ------------------------------------------------------------- sensors */
static void update_tilt(float dt)
{
    static bool cal;
    static float g0[3], gf[3];
    float a[3];
    if (!imu_ok || !imu_read_accel(&a[0], &a[1], &a[2])) return;
    if (!cal) { memcpy(g0, a, sizeof g0); memcpy(gf, a, sizeof gf); cal = true; }
    for (int i = 0; i < 3; i++) {
        gf[i] += (a[i] - gf[i]) * 0.25f;     // low-pass the hand
        g0[i] += (gf[i] - g0[i]) * dt / 8;   // and drift back to centre over ~8 s
    }
    float side = (gf[IMU_AXIS_SIDE] - g0[IMU_AXIS_SIDE]) * IMU_SIGN_SIDE;
    float fwd = (gf[IMU_AXIS_FWD] - g0[IMU_AXIS_FWD]) * IMU_SIGN_FWD;
    // +-6 px of vanishing point and +-2 degrees of roll, as in the concept
    scene.auto_tilt = false;
    scene.tilt.x = fmaxf(-6, fminf(6, side * 20));
    scene.tilt.y = fmaxf(-6, fminf(6, fwd * 20));
    scene.tilt.roll = fmaxf(-0.035f, fminf(0.035f, side * 0.12f));
}

static void update_battery(int64_t now)
{
    static int64_t next;
    static bool warned;
    if (now < next) return;
    next = now + 10 * 1000000LL;
    int pct = battery_percent(battery_mv());
    scene.battery_pct = pct;
    if (pct >= 0 && pct < 10 && !warned) { bs_toast(&scene, "LOW BATTERY", BS_CRIMSON, 2.5f); warned = true; }
    if (pct > 15) warned = false;
}

/* ------------------------------------------------------------ rendering */
static void render_helper(void *arg)
{
    for (;;) {
        xSemaphoreTake(helper_go, portMAX_DELAY);
        bs_render_shaft_rows(&frame_params, fb, 0, SPLIT_Y);
        xSemaphoreGive(helper_done);
    }
}

static void render_frame(void)
{
    bs_frame_params(&scene, &frame_params);
    xSemaphoreGive(helper_go);
    bs_render_shaft_rows(&frame_params, fb, SPLIT_Y, BS_H);
    xSemaphoreTake(helper_done, portMAX_DELAY);
    bs_render_overlays(&scene, &frame_params, fb);
}

static void display_task(void *arg)
{
    bs_init_tables();
    bs_scene_init(&scene, cfg.mode, (uint32_t)esp_timer_get_time() | 1);
    scene.splash_t0 = 0;

    display_init();
    keyboard_init();
    imu_ok = imu_init();
    battery_init();

    helper_go = xSemaphoreCreateBinary();
    helper_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(render_helper, "shaft", 4096, NULL, 2, NULL, 0);

    render_frame();
    display_push(fb);
    display_set_brightness((uint8_t)cfg.brightness);
    ESP_LOGI(TAG, "running: imu %d, free heap %u", imu_ok, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    int64_t prev = esp_timer_get_time();
    TickType_t wake = xTaskGetTickCount();
    uint32_t seen_activity = babel_activity;
    for (;;) {
        int64_t now = esp_timer_get_time();
        float dt = (now - prev) / 1e6f;
        prev = now;
        if (dt > 0.1f) dt = 0.1f;

        screen_ev_t ev;
        while (xQueueReceive(evq, &ev, 0) == pdTRUE) handle_event(&ev);
        key_event_t key;
        for (int i = 0; i < 4 && keyboard_poll(&key); i++) handle_key(&key);

        if (babel_activity != seen_activity) { seen_activity = babel_activity; scene.led_at = scene.t; }
        if (host_book_until && now > host_book_until) {
            host_book_until = 0;
            if (scene.book.is_open) bs_close(&scene);
        }
        if (scene.host == BS_HOST_LOOKUP && !scene.book.is_open && now - last_key_us > LOOKUP_IDLE_US) {
            lookup_len = 0;
            bs_disconnect(&scene);
            last_key_us = now;
        }
        update_tilt(dt);
        update_battery(now);

        bs_update(&scene, dt);
        scene.fps += (1.0f / fmaxf(dt, 0.001f) - scene.fps) * 0.1f;
        if (scene.show_fps) scene.free_kb = (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

        // Keep float time precise: start a fresh freefall after ~5 hours alone.
        if (scene.t > 18000 && !scene.host && !bs_queue_pending(&scene)) {
            bool fps = scene.show_fps;
            int bat = scene.battery_pct;
            bs_scene_init(&scene, cfg.mode, (uint32_t)now | 1);
            scene.show_fps = fps;
            scene.battery_pct = bat;
        }

        render_frame();
        display_push(fb);
        if (xTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_MS)) == pdFALSE) {
            vTaskDelay(1);   // running late: still let the idle task in
            wake = xTaskGetTickCount();
        }
    }
}

void screen_start(void)
{
    evq = xQueueCreate(32, sizeof(screen_ev_t));
    xTaskCreatePinnedToCore(display_task, "display", 6144, NULL, 3, NULL, 1);
}
