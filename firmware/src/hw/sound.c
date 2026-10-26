// Speaker: ES8311 codec on I2S, clocked from BCLK (no MCLK pin). The codec
// register sequence is M5Unified's _speaker_enabled_cb_cardputer_adv. Effects
// are synthesised on the fly by a low-priority task.
#include <math.h>
#include <string.h>

#include "board.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hw.h"

#define TAG "sound"
#define RATE 32000
#define CHUNK 256

enum { FX_TICK, FX_PAGE, FX_CHIME };

static i2s_chan_handle_t tx;
static QueueHandle_t fxq;
static volatile bool enabled = true;

static bool codec_init(void)
{
    i2c_master_dev_handle_t dev;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = BOARD_CODEC_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_probe(i2c_bus(), BOARD_CODEC_ADDR, 50) != ESP_OK) return false;
    if (i2c_master_bus_add_device(i2c_bus(), &cfg, &dev) != ESP_OK) return false;
    static const uint8_t seq[][2] = {
        { 0x00, 0x80 },  // reset / CSM power on
        { 0x01, 0xB5 },  // clock manager: MCLK = BCLK
        { 0x02, 0x18 },  // clock manager: MULT_PRE = 3
        { 0x0D, 0x01 },  // power up analog circuitry
        { 0x12, 0x00 },  // power up DAC
        { 0x13, 0x10 },  // enable output to HP drive
        { 0x32, 0xBF },  // DAC volume 0 dB
        { 0x37, 0x08 },  // bypass DAC equalizer
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++) ok &= i2c_master_transmit(dev, seq[i], 2, 50) == ESP_OK;
    return ok;
}

static void play(int fx)
{
    static int16_t buf[CHUNK * 2];
    static uint32_t noise = 0x1234567;
    int len = fx == FX_TICK ? RATE / 60 : fx == FX_PAGE ? RATE * 22 / 100 : RATE * 45 / 100;
    float lp = 0, ph1 = 0, ph2 = 0;
    for (int n = 0; n < len; n += CHUNK) {
        int m = len - n < CHUNK ? len - n : CHUNK;
        for (int i = 0; i < m; i++) {
            float t = (float)(n + i) / RATE, s = 0;
            noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
            float white = (int32_t)noise * (1.0f / 2147483648.0f);
            switch (fx) {
                case FX_TICK:   // a soft wooden tick
                    ph1 += 2 * 3.14159265f * 1800 / RATE;
                    s = sinf(ph1) * expf(-t * 380) * 0.22f;
                    break;
                case FX_PAGE: { // a page turning: filtered noise that swells and dies
                    lp += (white - lp) * 0.18f;
                    float env = t < 0.06f ? t / 0.06f : expf(-(t - 0.06f) * 22);
                    s = lp * env * 0.5f;
                    break;
                }
                case FX_CHIME:  // two lamp-glass tones
                    ph1 += 2 * 3.14159265f * 659.3f / RATE;
                    ph2 += 2 * 3.14159265f * 987.8f / RATE;
                    s = sinf(ph1) * expf(-t * 7) * 0.12f + (t > 0.12f ? sinf(ph2) * expf(-(t - 0.12f) * 7) * 0.1f : 0);
                    break;
            }
            int16_t v = (int16_t)(s * 32767);
            buf[i * 2] = buf[i * 2 + 1] = v;
        }
        size_t wrote;
        i2s_channel_write(tx, buf, m * 4, &wrote, portMAX_DELAY);
    }
}

static void sound_task(void *arg)
{
    int fx;
    for (;;) {
        if (xQueueReceive(fxq, &fx, portMAX_DELAY) == pdTRUE && enabled) play(fx);
    }
}

bool sound_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    cc.auto_clear = true;   // silence when nothing is queued
    if (i2s_new_channel(&cc, &tx, NULL) != ESP_OK) return false;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_BCK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) return false;
    if (!codec_init()) { ESP_LOGW(TAG, "ES8311 not found"); return false; }
    fxq = xQueueCreate(4, sizeof(int));
    xTaskCreatePinnedToCore(sound_task, "sound", 3072, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "ES8311 ready");
    return true;
}

void sound_set_enabled(bool on) { enabled = on; }
static void post(int fx) { if (fxq && enabled) xQueueSend(fxq, &fx, 0); }
void sound_tick(void) { post(FX_TICK); }
void sound_page(void) { post(FX_PAGE); }
void sound_chime(void) { post(FX_CHIME); }
