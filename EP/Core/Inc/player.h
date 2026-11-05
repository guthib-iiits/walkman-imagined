/**
  ******************************************************************************
  * @file    player.h
  * @brief   Playback engine: play queue, transport controls, crossfade, resume.
  ******************************************************************************
  */
#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include "mp3_stream.h"
#include "library.h"

typedef enum {
  PL_STOPPED = 0,
  PL_PLAYING,
  PL_PAUSED
} pl_state_t;

typedef struct {
  pl_state_t state;
  uint32_t index;
  uint32_t count;
  uint32_t pos_ms;
  uint32_t dur_ms;
  uint32_t rate;
  char path[LIB_PATH_LEN];
  const mp3_meta_t *meta;
  uint32_t track_serial;       /* changes whenever a new track starts */
} pl_info_t;

void player_init(void);
/* Starts the queue written by queue_from_*() at index, from pos_ms. */
bool player_start(uint32_t index, uint32_t pos_ms);
/* Plays one file from start_ms and stops at end_ms (mixtape clips). */
bool player_play_clip(const char *path, uint32_t start_ms, uint32_t end_ms);
void player_stop(void);
void player_toggle(void);
void player_pause(void);
void player_next(void);
void player_prev(void);
void player_seek_rel(int32_t delta_ms);
void player_volume_step(int dir);

/* Decodes ahead; call every main loop pass. */
void player_task(void);
void player_info(pl_info_t *i);
bool player_clip_done(void);
void player_save_resume(void);
/* Opens a file just to read its length; 0 if unknown or both decoders are busy. */
uint32_t player_probe_duration(const char *path);

#endif /* PLAYER_H */
