/**
  ******************************************************************************
  * @file    cs43131.h
  * @brief   Cirrus Logic CS43131 headphone DAC (x2), I2C control.
  *          Register map and power-up sequences follow the Linux cs43130 driver,
  *          which supports the CS43131.
  ******************************************************************************
  */
#ifndef CS43131_H
#define CS43131_H

#include <stdint.h>
#include <stdbool.h>

#define CS43131_NUM_DACS 2

/* Resets both DACs, checks their IDs and leaves them powered down.
 * SAI MCLK must already be running. Returns a bitmask of DACs found. */
uint8_t cs43131_init(void);

/* Programs the serial port for fs (44100 or 48000) with 32-bit I2S slots
 * and powers the output up. MCLK must be 512 x fs (24.576 / 22.5792 MHz). */
bool cs43131_start(uint32_t fs);
void cs43131_stop(void);

void cs43131_set_volume(uint8_t percent);   /* 0 = mute, 100 = 0 dB */
void cs43131_mute(bool mute);

#endif /* CS43131_H */
