/*
 * ThumbyNES — front RGB indicator helper.
 *
 * The Thumby Color's front indicator is three PWM-capable GPIOs wired
 * common-anode (0 = full on, 1 = off). The engine module normally
 * drives them via PWM slice 5/6; in the bare-metal slot we don't need
 * smooth fades — solid on/off is enough to signal "disk is writing,
 * don't power off" during the defragmenter. Using plain GPIO keeps
 * us from contending with any other PWM user in the slot.
 */
#ifndef THUMBYNES_LED_H
#define THUMBYNES_LED_H

void nes_led_init(void);
void nes_led_red(void);
void nes_led_off(void);

#endif
