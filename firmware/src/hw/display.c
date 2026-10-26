#include "display.h"

#include "board.h"
#include "bs_tables.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "display"

#define W 240
#define H 135
#define STRIP 15                 // 9 strips per frame
#define NBUF 2

static esp_lcd_panel_handle_t panel;
static uint16_t *strip_buf[NBUF];
static SemaphoreHandle_t buf_free;   // counts strip buffers the SPI driver has finished with
static uint16_t pal_be[16];          // palette, byte-swapped for the wire

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(buf_free, &woken);
    return woken == pdTRUE;
}

void display_set_brightness(uint8_t level)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 256,          // as M5GFX drives this panel's backlight
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t ch = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

void display_init(void)
{
    for (int i = 0; i < 16; i++) pal_be[i] = (uint16_t)((BS_PALETTE565[i] >> 8) | (BS_PALETTE565[i] << 8));
    backlight_init();

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = W * STRIP * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    buf_free = xSemaphoreCreateCounting(NBUF, NBUF);
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC,
        .cs_gpio_num = BOARD_LCD_CS,
        .pclk_hz = BOARD_LCD_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = NBUF + 2,
        .on_color_trans_done = on_color_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &dev, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, BOARD_LCD_GAP_X, BOARD_LCD_GAP_Y));

    for (int i = 0; i < NBUF; i++) {
        strip_buf[i] = heap_caps_malloc(W * STRIP * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(strip_buf[i]);
    }
    // Start black, then light the panel.
    for (int y = 0; y < H; y += STRIP) {
        xSemaphoreTake(buf_free, portMAX_DELAY);
        uint16_t *b = strip_buf[(y / STRIP) % NBUF];
        for (int i = 0; i < W * STRIP; i++) b[i] = pal_be[0];
        esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + STRIP, b);
    }
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_LOGI(TAG, "ST7789 ready");
}

void display_push(const uint8_t *fb)
{
    static int next = 0;
    for (int y = 0; y < H; y += STRIP) {
        xSemaphoreTake(buf_free, portMAX_DELAY);
        uint16_t *b = strip_buf[next];
        next = (next + 1) % NBUF;
        const uint8_t *src = fb + y * W;
        for (int i = 0; i < W * STRIP; i++) b[i] = pal_be[src[i] & 15];
        esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + STRIP, b);
    }
}
