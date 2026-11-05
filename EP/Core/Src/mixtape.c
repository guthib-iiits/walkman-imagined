/**
  ******************************************************************************
  * @file    mixtape.c
  * @brief   Mixtapes: play a clip of a song while recording the I2S mic, and
  *          save "voice over song" as a 16-bit stereo WAV on the SD card.
  *
  *  The song side comes from the audio tap (exactly what the DACs played) and
  *  the voice side from I2S2. Both run off PLL2/PLL3 at the same rate, so the
  *  two streams stay sample-aligned for the whole clip.
  ******************************************************************************
  */
#include "mixtape.h"
#include "audio_out.h"
#include "mic_in.h"
#include "player.h"
#include "storage.h"
#include "walkman_config.h"
#include <stdio.h>
#include <string.h>

#define CHUNK        1024U          /* frames mixed and written per loop */
#define SONG_GAIN    0.6f           /* song turned down so the voice is heard */
#define VOICE_GAIN   1.6f           /* the mic is quiet: boost it */
#define TMP_FILE     MIXTAPE_DIR "/recording.tmp"   /* renamed when saved */

/* States: IDLE -> RECORDING -> FINISHED -> (save or discard) -> IDLE,
 * or ERROR if something failed (err holds the message for the UI). */
static FIL wav;
static mt_state_t state = MT_IDLE;
static uint32_t rate, frames_written;
static bool tap_started;
static const char *err = "";
static int16_t song[CHUNK * 2U];    /* stereo, from the audio tap */
static int16_t voice[CHUNK];        /* mono, from the mic */
static int16_t out[CHUNK * 2U];     /* mixed stereo, written to the WAV */

/* WAV files are little-endian. */
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* The 44-byte WAV header. Written with size 0 at the start and rewritten
 * with the real size when the recording ends. */
static void write_header(uint32_t data_bytes)
{
  uint8_t h[44];
  UINT bw;
  memcpy(h, "RIFF", 4);
  put32(&h[4], 36U + data_bytes);
  memcpy(&h[8], "WAVEfmt ", 8);
  put32(&h[16], 16);
  put16(&h[20], 1);                 /* PCM */
  put16(&h[22], 2);                 /* stereo */
  put32(&h[24], rate);
  put32(&h[28], rate * 4U);
  put16(&h[32], 4);
  put16(&h[34], 16);
  memcpy(&h[36], "data", 4);
  put32(&h[40], data_bytes);
  f_lseek(&wav, 0);
  f_write(&wav, h, sizeof(h), &bw);
}

static bool fail(const char *why)
{
  err = why;
  state = MT_ERROR;
  return false;
}

/* Start a mixtape: play song_path from start_ms to end_ms and record the mic
 * over it. Returns false (and sets the error message) if it can't start. */
bool mixtape_begin(const char *song_path, uint32_t start_ms, uint32_t end_ms)
{
  if (!storage_sd_mounted())
  {
    storage_sd_rescan();
    if (!storage_sd_mounted())
    {
      return fail("Insert SD card");
    }
  }
  storage_mkdirs(MIXTAPE_DIR);
  if (f_open(&wav, TMP_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
  {
    return fail("SD write error");
  }
  if (!player_play_clip(song_path, start_ms, end_ms))
  {
    f_close(&wav);
    return fail("Can't play song");
  }
  pl_info_t i;
  player_info(&i);
  rate = i.rate;
  write_header(0);
  frames_written = 0;

  if (!mic_start(rate))
  {
    player_stop();
    f_close(&wav);
    return fail("Mic error");
  }
  audio_tap_enable(true);
  tap_started = false;
  state = MT_RECORDING;
  return true;
}

/* Limit to the 16-bit range so loud parts clip instead of wrapping around. */
static inline int16_t clip16(float v)
{
  if (v > 32767.0f)
  {
    return 32767;
  }
  if (v < -32768.0f)
  {
    return -32768;
  }
  return (int16_t)v;
}

/* Take everything played and recorded so far, mix voice into both channels
 * of the song, and append it to the WAV file. */
static void drain(void)
{
  for (;;)
  {
    uint32_t n = audio_tap_read(song, CHUNK);
    if (n == 0U)
    {
      return;
    }
    if (!tap_started)
    {
      mic_discard();                /* line the voice up with the first played sample */
      tap_started = true;
    }
    uint32_t m = mic_read(voice, n);
    for (uint32_t k = m; k < n; k++)
    {
      voice[k] = 0;
    }
    for (uint32_t k = 0; k < n; k++)
    {
      float v = (float)voice[k] * VOICE_GAIN;
      out[2U * k]      = clip16((float)song[2U * k] * SONG_GAIN + v);
      out[2U * k + 1U] = clip16((float)song[2U * k + 1U] * SONG_GAIN + v);
    }
    UINT bw;
    if (f_write(&wav, out, n * 4U, &bw) != FR_OK || bw != n * 4U)
    {
      mixtape_stop();
      fail("SD card full");
      return;
    }
    frames_written += n;
  }
}

static void finish(void)
{
  audio_tap_enable(false);
  mic_stop();
  write_header(frames_written * 4U);
  f_close(&wav);
  state = MT_FINISHED;
}

/* Main loop: keep writing while recording; finish when the clip has played out. */
void mixtape_task(void)
{
  if (state != MT_RECORDING)
  {
    return;
  }
  drain();
  if (state == MT_RECORDING && player_clip_done() && audio_out_queued() == 0U)
  {
    HAL_Delay(15);                  /* last DMA half */
    drain();
    finish();
  }
}

void mixtape_stop(void)
{
  if (state != MT_RECORDING)
  {
    return;
  }
  player_stop();
  drain();
  finish();
}

/* Keep the finished recording as the next free "MixTape NNN.wav". */
bool mixtape_save(char *name_out, uint32_t cap)
{
  char path[96];
  if (state != MT_FINISHED)
  {
    return false;
  }
  for (int n = 1; n < 1000; n++)
  {
    snprintf(path, sizeof(path), MIXTAPE_DIR "/MixTape %03d.wav", n);
    if (!storage_exists(path))
    {
      state = MT_IDLE;
      if (f_rename(TMP_FILE, path) != FR_OK)
      {
        return false;
      }
      snprintf(name_out, cap, "MixTape %03d", n);
      return true;
    }
  }
  return false;
}

void mixtape_discard(void)
{
  if (state == MT_RECORDING)
  {
    mixtape_stop();
  }
  f_unlink(TMP_FILE);
  state = MT_IDLE;
}

mt_state_t mixtape_state(void)
{
  return state;
}

uint32_t mixtape_elapsed_ms(void)
{
  return rate ? (uint32_t)((uint64_t)frames_written * 1000U / rate) : 0U;
}

/* Mic level 0..100 for the on-screen meter. */
uint8_t mixtape_level(void)
{
  uint32_t p = mic_peak();
  return (uint8_t)(p * 100U / 32768U);
}

const char *mixtape_error(void)
{
  return err;
}
