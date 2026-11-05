/**
  ******************************************************************************
  * @file    mic_in.h
  * @brief   16-bit I2S microphone on I2S2 (master RX, circular DMA).
  ******************************************************************************
  */
#ifndef MIC_IN_H
#define MIC_IN_H

#include <stdint.h>
#include <stdbool.h>

bool     mic_start(uint32_t fs);      /* clocks the mic at the song's rate */
void     mic_stop(void);
uint32_t mic_read(int16_t *mono, uint32_t max_samples);
void     mic_discard(void);
uint16_t mic_peak(void);              /* recent peak level, for the record meter */

#endif /* MIC_IN_H */
