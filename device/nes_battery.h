/*
 * ThumbyNES — Thumby Color battery monitor.
 *
 * The board exposes the battery through a 1:2 resistor divider into
 * GPIO 29 / ADC channel 3. ADC reads the half-voltage; the wrapper
 * doubles it to report the full pack voltage. Charging is detected
 * when the half-voltage saturates above the maximum (i.e. external
 * power is holding the input rail above the cell's max).
 *
 * Numeric thresholds match the engine's reference implementation
 * (engine_io_rp3.c) so behavior on hardware is identical.
 */
#ifndef THUMBYNES_BATTERY_H
#define THUMBYNES_BATTERY_H

#include <stdbool.h>

void  nes_battery_init     (void);
float nes_battery_voltage  (void);   /* 2.6..4.2 V on a healthy pack */
int   nes_battery_percent  (void);   /* 0..100; clamped              */
bool  nes_battery_charging (void);

#endif
