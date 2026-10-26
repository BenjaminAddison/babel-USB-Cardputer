// Cardputer ADV keyboard: a TCA8418 scanning a 7 x 8 matrix, remapped to the
// original Cardputer's 4 x 14 layout. Key map and remap follow M5Stack's
// M5Cardputer library (MIT, Copyright (c) 2025 M5Stack Technology Co., Ltd.).
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hw.h"

#define TAG "keyboard"

#define REG_CFG          0x01
#define REG_INT_STAT     0x02
#define REG_KEY_LCK_EC   0x03
#define REG_KEY_EVENT_A  0x04
#define REG_GPIO_INT_STAT_1 0x11
#define REG_GPIO_INT_EN_1 0x1A
#define REG_KP_GPIO_1    0x1D
#define REG_GPI_EM_1     0x20
#define REG_GPIO_DIR_1   0x23
#define REG_GPIO_INT_LVL_1 0x26

static i2c_master_dev_handle_t dev;
static bool fn, shift, ctrl, opt, alt;
static bool btn_was_down;
static int64_t btn_changed_us;

// [row][col] in the original Cardputer layout: normal, shifted, fn layer.
typedef struct { uint8_t normal, shifted, fn; } keydef_t;
static const keydef_t MAP[4][14] = {
    { {'`', '~', KEY_ESC}, {'1', '!', 0}, {'2', '@', 0}, {'3', '#', 0}, {'4', '$', 0}, {'5', '%', 0}, {'6', '^', 0},
      {'7', '&', 0}, {'8', '*', 0}, {'9', '(', 0}, {'0', ')', 0}, {'-', '_', 0}, {'=', '+', 0}, {KEY_BKSP, KEY_BKSP, KEY_DEL} },
    { {KEY_TAB, KEY_TAB, KEY_TAB}, {'q', 'Q', 0}, {'w', 'W', 0}, {'e', 'E', 0}, {'r', 'R', 0}, {'t', 'T', 0}, {'y', 'Y', 0},
      {'u', 'U', 0}, {'i', 'I', 0}, {'o', 'O', 0}, {'p', 'P', 0}, {'[', '{', 0}, {']', '}', 0}, {'\\', '|', 0} },
    { {KEY_FN, KEY_FN, KEY_FN}, {KEY_SHIFT, KEY_SHIFT, KEY_SHIFT}, {'a', 'A', 0}, {'s', 'S', 0}, {'d', 'D', 0}, {'f', 'F', 0},
      {'g', 'G', 0}, {'h', 'H', 0}, {'j', 'J', 0}, {'k', 'K', 0}, {'l', 'L', 0}, {';', ':', KEY_UP}, {'\'', '"', 0}, {KEY_ENTER, KEY_ENTER, KEY_ENTER} },
    { {KEY_CTRL, KEY_CTRL, KEY_CTRL}, {KEY_OPT, KEY_OPT, KEY_OPT}, {KEY_ALT, KEY_ALT, KEY_ALT}, {'z', 'Z', 0}, {'x', 'X', 0},
      {'c', 'C', 0}, {'v', 'V', 0}, {'b', 'B', 0}, {'n', 'N', 0}, {'m', 'M', 0}, {',', '<', KEY_LEFT}, {'.', '>', KEY_DOWN},
      {'/', '?', KEY_RIGHT}, {' ', ' ', ' '} },
};

static bool wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(dev, b, 2, 20) == ESP_OK;
}
static int rd(uint8_t reg)
{
    uint8_t v;
    if (i2c_master_transmit_receive(dev, &reg, 1, &v, 1, 20) != ESP_OK) return -1;
    return v;
}

bool keyboard_init(void)
{
    gpio_config_t btn = { .pin_bit_mask = 1ULL << BOARD_BTN_A, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&btn);

    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = BOARD_KB_ADDR, .scl_speed_hz = BOARD_I2C_HZ };
    if (i2c_master_bus_add_device(i2c_bus(), &cfg, &dev) != ESP_OK) return false;
    if (i2c_master_probe(i2c_bus(), BOARD_KB_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "TCA8418 not found");
        return false;
    }
    for (int i = 0; i < 3; i++) {
        wr(REG_GPIO_DIR_1 + i, 0x00);       // all inputs
        wr(REG_GPI_EM_1 + i, 0xFF);         // GPIs into the event FIFO
        wr(REG_GPIO_INT_LVL_1 + i, 0x00);
        wr(REG_GPIO_INT_EN_1 + i, 0xFF);
    }
    wr(REG_KP_GPIO_1, 0x7F);                // rows 0..6
    wr(REG_KP_GPIO_1 + 1, 0xFF);            // cols 0..7
    wr(REG_KP_GPIO_1 + 2, 0x00);
    while (rd(REG_KEY_EVENT_A) > 0) {}      // flush
    for (int i = 0; i < 3; i++) rd(REG_GPIO_INT_STAT_1 + i);
    wr(REG_INT_STAT, 0x03);
    wr(REG_CFG, 0x01);                      // key event interrupt (the FIFO works either way)
    ESP_LOGI(TAG, "TCA8418 ready");
    return true;
}

static bool decode(uint8_t raw, key_event_t *ev)
{
    bool down = raw & 0x80;
    int k = (raw & 0x7F) - 1;
    if (k < 0 || k >= 70) return false;     // GPI events and noise
    int trow = k / 10, tcol = k % 10;
    int col = trow * 2 + (tcol > 3 ? 1 : 0);
    int row = tcol % 4;
    if (row > 3 || col > 13) return false;
    const keydef_t *d = &MAP[row][col];
    switch (d->normal) {
        case KEY_FN: fn = down; break;
        case KEY_SHIFT: shift = down; break;
        case KEY_CTRL: ctrl = down; break;
        case KEY_OPT: opt = down; break;
        case KEY_ALT: alt = down; break;
        default: break;
    }
    if (!down) return false;
    uint8_t code = fn && d->fn ? d->fn : shift ? d->shifted : d->normal;
    *ev = (key_event_t){ .code = code, .fn = fn, .shift = shift, .ctrl = ctrl, .opt = opt, .alt = alt };
    return code != KEY_NONE;
}

bool keyboard_poll(key_event_t *ev)
{
    // BtnA, debounced
    bool btn = gpio_get_level(BOARD_BTN_A) == 0;
    int64_t now = esp_timer_get_time();
    if (btn != btn_was_down && now - btn_changed_us > 30000) {
        btn_was_down = btn;
        btn_changed_us = now;
        if (btn) { *ev = (key_event_t){ .code = KEY_BTN_A }; return true; }
    }
    if (!dev) return false;
    int n = rd(REG_KEY_LCK_EC);
    if (n <= 0 || (n & 0x0F) == 0) return false;
    for (int i = n & 0x0F; i > 0; i--) {
        int raw = rd(REG_KEY_EVENT_A);
        if (raw <= 0) break;
        if (decode((uint8_t)raw, ev)) {
            wr(REG_INT_STAT, 0x01);
            return true;
        }
    }
    wr(REG_INT_STAT, 0x01);
    return false;
}
