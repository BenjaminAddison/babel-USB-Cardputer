// BMI270 accelerometer, enough of it to tilt the shaft. The init sequence
// follows M5Stack's M5Unified BMI270_Class (MIT): soft reset, upload the config
// blob, wait for the sensor to report ready, then switch the accelerometer on.
#include "bmi270_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw.h"

#define TAG "imu"

#define REG_CHIP_ID        0x00
#define REG_ACC_X_LSB      0x0C
#define REG_INTERNAL_STATUS 0x21
#define REG_ACC_CONF       0x40
#define REG_ACC_RANGE      0x41
#define REG_INIT_CTRL      0x59
#define REG_INIT_ADDR_0    0x5B
#define REG_INIT_DATA      0x5E
#define REG_PWR_CONF       0x7C
#define REG_PWR_CTRL       0x7D
#define REG_CMD            0x7E

static i2c_master_dev_handle_t dev;

static bool wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(dev, b, 2, 50) == ESP_OK;
}
static int rd(uint8_t reg)
{
    uint8_t v;
    if (i2c_master_transmit_receive(dev, &reg, 1, &v, 1, 50) != ESP_OK) return -1;
    return v;
}

bool imu_init(void)
{
    static const uint8_t addrs[2] = { 0x69, 0x68 };
    uint8_t addr = 0;
    for (int i = 0; i < 2 && !addr; i++)
        if (i2c_master_probe(i2c_bus(), addrs[i], 50) == ESP_OK) addr = addrs[i];
    if (!addr) { ESP_LOGW(TAG, "BMI270 not found"); return false; }
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(i2c_bus(), &cfg, &dev) != ESP_OK) return false;
    if (rd(REG_CHIP_ID) != 0x24) { ESP_LOGW(TAG, "unexpected chip id"); return false; }

    wr(REG_CMD, 0xB6);                         // soft reset
    vTaskDelay(pdMS_TO_TICKS(3));
    wr(REG_PWR_CONF, 0x00);                    // advanced power save off
    vTaskDelay(pdMS_TO_TICKS(1));
    wr(REG_INIT_CTRL, 0x00);
    static uint8_t burst[1 + 256];
    for (int off = 0; off < (int)sizeof bmi270_config_file; off += 256) {
        uint8_t a[3] = { REG_INIT_ADDR_0, (uint8_t)((off >> 1) & 0x0F), (uint8_t)(off >> 5) };
        i2c_master_transmit(dev, a, 3, 50);
        burst[0] = REG_INIT_DATA;
        for (int i = 0; i < 256; i++) burst[1 + i] = bmi270_config_file[off + i];
        if (i2c_master_transmit(dev, burst, sizeof burst, 100) != ESP_OK) { ESP_LOGW(TAG, "config upload failed"); return false; }
    }
    wr(REG_INIT_CTRL, 0x01);
    int st = 0;
    for (int i = 0; i < 30 && (st & 0x0F) != 1; i++) { vTaskDelay(pdMS_TO_TICKS(5)); st = rd(REG_INTERNAL_STATUS); }
    if ((st & 0x0F) != 1) ESP_LOGW(TAG, "init status %d", st);
    wr(REG_PWR_CTRL, 0x04);                    // accelerometer on
    wr(REG_ACC_CONF, 0xA8);                    // 100 Hz, normal filter, performance mode
    wr(REG_ACC_RANGE, 0x02);                   // +-8 g
    ESP_LOGI(TAG, "BMI270 at 0x%02x", addr);
    return true;
}

bool imu_read_accel(float *ax, float *ay, float *az)
{
    if (!dev) return false;
    uint8_t reg = REG_ACC_X_LSB, b[6];
    if (i2c_master_transmit_receive(dev, &reg, 1, b, 6, 20) != ESP_OK) return false;
    const float s = 8.0f / 32768.0f;
    *ax = (int16_t)(b[0] | b[1] << 8) * s;
    *ay = (int16_t)(b[2] | b[3] << 8) * s;
    *az = (int16_t)(b[4] | b[5] << 8) * s;
    return true;
}
