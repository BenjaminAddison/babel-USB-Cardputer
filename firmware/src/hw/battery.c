#include "board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "hw.h"

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
static adc_channel_t chan;

void battery_init(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(BOARD_BAT_GPIO, &unit, &chan) != ESP_OK) return;
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    if (adc_oneshot_new_unit(&ucfg, &adc) != ESP_OK) { adc = NULL; return; }
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(adc, chan, &ccfg);
    adc_cali_curve_fitting_config_t cc = { .unit_id = unit, .chan = chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cc, &cali) != ESP_OK) cali = NULL;
}

int battery_mv(void)
{
    if (!adc) return 0;
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw = 0, mv = 0;
        adc_oneshot_read(adc, chan, &raw);
        if (cali) adc_cali_raw_to_voltage(cali, raw, &mv);
        else mv = raw * 3100 / 4095;
        sum += mv;
    }
    return sum / 8 * 2;   // 1:2 divider
}

int battery_percent(int mv)
{
    if (mv <= 0) return -1;
    int p = (mv - 3300) * 100 / (4150 - 3300);
    return p < 0 ? 0 : p > 100 ? 100 : p;
}
