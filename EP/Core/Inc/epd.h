/**
  ******************************************************************************
  * @file    epd.h
  * @brief   Waveshare 2.13" e-Paper (B) V3 driver (UC8151D, 104 x 212, B/W/R).
  *
  *  Two ways to update the glass:
  *   - colour: factory OTP waveform, black + red, whole panel, ~15 s
  *   - fast:   black/white register waveform inside a window, ~0.5 s. Pixels
  *             that stay the same get no drive, so red already on the glass
  *             survives. Not part of Waveshare's spec for the tri-colour panel.
  *  display.c decides which one to use.
  ******************************************************************************
  */
#ifndef EPD_H
#define EPD_H

#include <stdint.h>
#include <stdbool.h>

#define EPD_WIDTH       104U
#define EPD_HEIGHT      212U
#define EPD_ROW_BYTES   (EPD_WIDTH / 8U)
#define EPD_PLANE_BYTES (EPD_ROW_BYTES * EPD_HEIGHT)

void epd_init(void);
bool epd_busy(void);

/* Plane bit = 1 means ink (black in k, red in r). Both return as soon as the
 * refresh has started; poll epd_busy(). */
void epd_colour_start(const uint8_t *k, const uint8_t *r);
/* Window in bytes (x) and rows (y), inclusive. old_k is what is black on the
 * glass now, new_k what should be black afterwards. */
void epd_fast_start(const uint8_t *old_k, const uint8_t *new_k,
                    uint8_t xb0, uint8_t xb1, uint16_t y0, uint16_t y1);
void epd_fast_finish(void);

/* Cuts a refresh short (hardware reset). The glass content is then undefined. */
void epd_abort(void);
void epd_sleep(void);

#endif /* EPD_H */
