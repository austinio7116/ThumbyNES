/*
 * ThumbyNES — front RGB indicator helper (bare-metal GPIO).
 *
 * Pin map (mirrors engine_io_rp3.h):
 *   GP10 = green, GP11 = red, GP12 = blue. Common anode, so driving
 *   the pin LOW turns a channel on.
 */
#include "nes_led.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#define LED_R_GP 11
#define LED_G_GP 10
#define LED_B_GP 12

void nes_led_init(void) {
    gpio_init(LED_R_GP);
    gpio_init(LED_G_GP);
    gpio_init(LED_B_GP);
    gpio_set_dir(LED_R_GP, GPIO_OUT);
    gpio_set_dir(LED_G_GP, GPIO_OUT);
    gpio_set_dir(LED_B_GP, GPIO_OUT);
    nes_led_off();
}

void nes_led_red(void) {
    nes_led_init();
    gpio_put(LED_R_GP, 0);
    gpio_put(LED_G_GP, 1);
    gpio_put(LED_B_GP, 1);
}

void nes_led_off(void) {
    gpio_put(LED_R_GP, 1);
    gpio_put(LED_G_GP, 1);
    gpio_put(LED_B_GP, 1);
}
