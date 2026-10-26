#pragma once

// M5Stack Cardputer ADV (Stamp-S3A, ESP32-S3FN8, 8 MB flash, no PSRAM).
// Pins from M5GFX / M5Unified / M5Cardputer board support.

// ST7789V2, 135 x 240, used in landscape (240 x 135)
#define BOARD_LCD_HOST      SPI3_HOST
#define BOARD_LCD_MOSI      35
#define BOARD_LCD_SCLK      36
#define BOARD_LCD_DC        34
#define BOARD_LCD_CS        37
#define BOARD_LCD_RST       33
#define BOARD_LCD_BL        38
#define BOARD_LCD_HZ        (40 * 1000 * 1000)
#define BOARD_LCD_GAP_X     40
#define BOARD_LCD_GAP_Y     53

// Internal I2C: TCA8418 keyboard (0x34), BMI270 IMU (0x69 or 0x68), ES8311 codec (0x18)
#define BOARD_I2C_SDA       8
#define BOARD_I2C_SCL       9
#define BOARD_I2C_HZ        400000
#define BOARD_KB_ADDR       0x34
#define BOARD_KB_INT        11
#define BOARD_CODEC_ADDR    0x18

// I2S to the ES8311 (speaker)
#define BOARD_I2S_BCK       41
#define BOARD_I2S_WS        43
#define BOARD_I2S_DOUT      42

// Battery: ADC1 on GPIO10 through a 1:2 divider
#define BOARD_BAT_GPIO      10

// BtnA ("G0"), active low
#define BOARD_BTN_A         0
