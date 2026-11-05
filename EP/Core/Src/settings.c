/**
  ******************************************************************************
  * @file    settings.c
  * @brief   User preferences (eMMC /System/settings.cfg) and resume state.
  *          Plain "key=value" lines so the files can be edited over USB.
  ******************************************************************************
  */
#include "settings.h"
#include "storage.h"
#include "walkman_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

settings_t g_settings;             /* the live settings, read everywhere */

/* Values used on first boot, or for any key missing from the file. */
static void defaults(void)
{
  memset(&g_settings, 0, sizeof(g_settings));
  g_settings.audio_32bit = true;
  g_settings.wallpaper = true;
  g_settings.hysteresis = true;
  g_settings.progress_bar = true;
  g_settings.show_time = true;
  g_settings.volume = AUDIO_DEFAULT_VOLUME;
  strcpy(g_settings.custom_text, "EP Walkman");
}

/* One row per setting: its name in the file, its type and where it lives in
 * g_settings. To add a setting, add it to settings_t, defaults() and here. */
typedef struct {
  const char *key;
  enum { T_BOOL, T_U8, T_STR } type;
  void *ptr;
  size_t size;                     /* buffer size, strings only */
} field_t;

static const field_t fields[] = {
  { "audio_32bit",   T_BOOL, &g_settings.audio_32bit,   0 },
  { "wallpaper",     T_BOOL, &g_settings.wallpaper,     0 },
  { "hysteresis",    T_BOOL, &g_settings.hysteresis,    0 },
  { "sleep_min",     T_U8,   &g_settings.sleep_min,     0 },
  { "progress_bar",  T_BOOL, &g_settings.progress_bar,  0 },
  { "crossfade",     T_BOOL, &g_settings.crossfade,     0 },
  { "high_contrast", T_BOOL, &g_settings.high_contrast, 0 },
  { "low_power",     T_BOOL, &g_settings.low_power,     0 },
  { "show_time",     T_BOOL, &g_settings.show_time,     0 },
  { "shuffle",       T_BOOL, &g_settings.shuffle,       0 },
  { "repeat",        T_BOOL, &g_settings.repeat,        0 },
  { "volume",        T_U8,   &g_settings.volume,        0 },
  { "custom_text",   T_STR,  g_settings.custom_text,    sizeof(g_settings.custom_text) },
};

/* Strip the line ending and trailing spaces (Windows editors add "\r\n"). */
static void trim(char *s)
{
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
  {
    s[--n] = '\0';
  }
}

/* Start from the defaults, then apply every "key=value" line found in the
 * file. Unknown keys are ignored, so old files keep working. */
void settings_load(void)
{
  FIL f;
  char line[64];

  defaults();
  if (f_open(&f, SETTINGS_FILE, FA_READ) != FR_OK)
  {
    return;
  }
  while (f_gets(line, sizeof(line), &f))
  {
    trim(line);
    char *eq = strchr(line, '=');
    if (!eq)
    {
      continue;
    }
    *eq = '\0';
    const char *val = eq + 1;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
    {
      if (strcmp(line, fields[i].key) != 0)
      {
        continue;
      }
      switch (fields[i].type)
      {
        case T_BOOL: *(bool *)fields[i].ptr = atoi(val) != 0; break;
        case T_U8:   *(uint8_t *)fields[i].ptr = (uint8_t)atoi(val); break;
        case T_STR:
          strncpy((char *)fields[i].ptr, val, fields[i].size - 1);
          ((char *)fields[i].ptr)[fields[i].size - 1] = '\0';
          break;
      }
    }
  }
  f_close(&f);
  if (g_settings.volume > 100U)
  {
    g_settings.volume = AUDIO_DEFAULT_VOLUME;
  }
}

/* Rewrite the whole file from g_settings. */
void settings_save(void)
{
  FIL f;
  if (!storage_emmc_mounted() || f_open(&f, SETTINGS_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
  {
    return;
  }
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
  {
    switch (fields[i].type)
    {
      case T_BOOL: f_printf(&f, "%s=%d\n", fields[i].key, *(bool *)fields[i].ptr ? 1 : 0); break;
      case T_U8:   f_printf(&f, "%s=%d\n", fields[i].key, *(uint8_t *)fields[i].ptr); break;
      case T_STR:  f_printf(&f, "%s=%s\n", fields[i].key, (char *)fields[i].ptr); break;
    }
  }
  f_close(&f);
}

/* "Hysteresis": where playback stopped last time (position in the queue and
 * in the song), saved at shutdown and loaded at boot. */
void resume_load(resume_t *r)
{
  FIL f;
  char line[48];
  memset(r, 0, sizeof(*r));
  if (f_open(&f, RESUME_FILE, FA_READ) != FR_OK)
  {
    return;
  }
  while (f_gets(line, sizeof(line), &f))
  {
    trim(line);
    if (strncmp(line, "index=", 6) == 0)
    {
      r->queue_index = (uint32_t)strtoul(line + 6, NULL, 10);
      r->valid = true;
    }
    else if (strncmp(line, "pos_ms=", 7) == 0)
    {
      r->position_ms = (uint32_t)strtoul(line + 7, NULL, 10);
    }
  }
  f_close(&f);
}

void resume_save(const resume_t *r)
{
  FIL f;
  if (!storage_emmc_mounted() || f_open(&f, RESUME_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
  {
    return;
  }
  f_printf(&f, "index=%lu\npos_ms=%lu\n", (unsigned long)r->queue_index, (unsigned long)r->position_ms);
  f_close(&f);
}
