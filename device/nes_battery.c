/*
 * ThumbyNES — Thumby Color battery monitor (RP2350 ADC).
 */
#include "nes_battery.h"

#include "pico/stdlib.h"
#include "hardware/adc.h"

#define BATT_GPIO     29
#define BATT_ADC_CH    3

#define ADC_REF_VOLTS  3.3f
#define ADC_MAX_COUNT  4095

/* Half-voltage thresholds: the divider gives the ADC half the cell
 * voltage. POWER_MIN/MAX values copied from engine_defines.h so the
 * percent reading matches the engine UI. */
#define HALF_MIN_V     (1.4f + 0.05f)   /* ~2.9 V actual (LiPo cutoff)  */
#define HALF_MAX_V     (2.0f - 0.15f)   /* ~3.7 V actual (charge full)  */

static int s_initialized = 0;

void nes_battery_init(void)
{
    if (s_initialized) return;
    adc_init();
    adc_gpio_init(BATT_GPIO);
    s_initialized = 1;
}

static float read_half_voltage(void)
{
    nes_battery_init();
    adc_select_input(BATT_ADC_CH);
    /* The first conversion after a channel switch on the RP2040/2350
     * ADC returns the previously-selected channel's sample (it was
     * already in flight when we changed inputs). Discard it and take
     * the second read, which is from the channel we actually asked
     * for. Without this the picker menu's Battery row only updates
     * on the *second* time you open the menu. */
    (void)adc_read();
    uint16_t raw = adc_read();
    return (float)raw * ADC_REF_VOLTS / (float)ADC_MAX_COUNT;
}

float nes_battery_voltage(void)
{
    return 2.0f * read_half_voltage();
}

int nes_battery_percent(void)
{
    float h = read_half_voltage();
    if (h <= HALF_MIN_V) return 0;
    if (h >= HALF_MAX_V) return 100;
    float frac = (h - HALF_MIN_V) / (HALF_MAX_V - HALF_MIN_V);
    int pct = (int)(frac * 100.0f + 0.5f);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

bool nes_battery_charging(void)
{
    return read_half_voltage() >= HALF_MAX_V;
}
