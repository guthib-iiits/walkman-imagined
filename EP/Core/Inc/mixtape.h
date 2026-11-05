/**
  ******************************************************************************
  * @file    mixtape.h
  * @brief   Mixtapes: play a clip of a song while recording the I2S mic, and
  *          save "voice over song" as a 16-bit stereo WAV on the SD card.
  ******************************************************************************
  */
#ifndef MIXTAPE_H
#define MIXTAPE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
  MT_IDLE = 0,
  MT_RECORDING,
  MT_FINISHED,          /* waiting for save / discard */
  MT_ERROR
} mt_state_t;

bool       mixtape_begin(const char *song, uint32_t start_ms, uint32_t end_ms);
void       mixtape_task(void);
void       mixtape_stop(void);               /* end early, keep what was recorded */
bool       mixtape_save(char *name_out, uint32_t cap);
void       mixtape_discard(void);
mt_state_t mixtape_state(void);
uint32_t   mixtape_elapsed_ms(void);
uint8_t    mixtape_level(void);              /* mic level 0..100 */
const char *mixtape_error(void);

#endif /* MIXTAPE_H */
