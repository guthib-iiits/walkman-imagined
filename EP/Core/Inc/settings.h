/**
  ******************************************************************************
  * @file    settings.h
  * @brief   User preferences (eMMC /System/settings.cfg) and resume state.
  ******************************************************************************
  */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
  bool     audio_32bit;      /* Audio Quality: 32-bit (true) or 16-bit output */
  bool     wallpaper;
  bool     hysteresis;       /* resume last session on boot */
  uint8_t  sleep_min;        /* 0 = off, else minutes */
  bool     progress_bar;
  bool     crossfade;
  bool     high_contrast;    /* no pink, black and white only */
  bool     low_power;
  bool     show_time;
  bool     shuffle;
  bool     repeat;
  uint8_t  volume;
  char     custom_text[24];
} settings_t;

typedef struct {
  bool     valid;
  uint32_t queue_index;
  uint32_t position_ms;
} resume_t;

extern settings_t g_settings;

void settings_load(void);
void settings_save(void);

void resume_load(resume_t *r);
void resume_save(const resume_t *r);

#endif /* SETTINGS_H */
