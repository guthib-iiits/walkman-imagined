/**
  ******************************************************************************
  * @file    audio_out.h
  * @brief   SAI1 A + B output to both CS43131s through one shared DMA buffer.
  *          The main loop writes 32-bit stereo frames into a ring buffer; the
  *          DMA half/complete interrupts move them into the SAI buffer.
  ******************************************************************************
  */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdint.h>
#include <stdbool.h>

#define AUDIO_RING_FRAMES 8192U        /* ~170 ms at 48 kHz */

bool     audio_out_init(void);          /* SAI running at 48 kHz, DACs muted */
/* Switches PLL2, SAI and both DACs to fs (44100 or 48000). */
bool     audio_out_set_rate(uint32_t fs);
uint32_t audio_out_rate(void);

uint32_t audio_out_space(void);         /* frames that can be written */
uint32_t audio_out_queued(void);        /* frames waiting to play */
void     audio_out_write(const int32_t *lr, uint32_t frames);
void     audio_out_flush(void);         /* drop everything queued */
/* Pause: play silence but keep the queued audio, so resume is seamless. */
void     audio_out_hold(bool hold);
uint32_t audio_out_underruns(void);

void     audio_out_set_volume(uint8_t percent);
uint8_t  audio_out_volume(void);

/* Low power: stop the SAI clocks and power the DACs down. */
void     audio_out_suspend(void);
void     audio_out_resume(void);
bool     audio_out_suspended(void);

/* Mixtape tap: a copy of exactly what is being played, as 16-bit stereo. */
void     audio_tap_enable(bool on);
uint32_t audio_tap_read(int16_t *lr, uint32_t max_frames);

/* PLL3 feeds SPI1/I2S2; the mic needs it on the same 48 k / 44.1 k family. */
void     audio_clock_set_i2s_family(uint32_t fs);

#endif /* AUDIO_OUT_H */
