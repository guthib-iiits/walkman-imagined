/**
  ******************************************************************************
  * @file    player.c
  * @brief   Playback engine: play queue, transport controls, crossfade, resume.
  *
  *  Two decoders: "cur" is the playing track, "nxt" is only open during a
  *  crossfade. Decoded frames go through small staging buffers so the two
  *  streams can be mixed sample by sample even though MP3 frames differ in size.
  *
  *  player_task() runs from the main loop and keeps the audio ring buffer
  *  full: decode a frame, write it, repeat until the ring has no room. The
  *  ring holds ~170 ms, so the loop can be busy elsewhere for a moment
  *  (screen redraws, file access) without the music stopping.
  *
  *  "Clip mode" plays one song from start_ms to end_ms for a mixtape.
  ******************************************************************************
  */
#include "player.h"
#include "audio_out.h"
#include "settings.h"
#include "walkman_config.h"
#include <string.h>

#define CROSSFADE_MS   3000U        /* overlap between songs when crossfade is on */
#define HISTORY        16U          /* songs remembered for "previous" in shuffle */
#define RESUME_SAVE_MS 30000U       /* save the resume point this often */

/* Decoded audio waiting to be written to the ring. */
typedef struct {
  int32_t buf[MP3S_MAX_OUT_FRAMES * 2U];
  uint32_t len, pos;                /* frames in buf / frames already used */
} stage_t;

static mp3s_t dec[2];
static stage_t stage[2];
static int cur = 0;                 /* index into dec/stage of the playing track */
static bool fading;                 /* crossfade running: dec[cur ^ 1] is the next song */
static uint32_t fade_done, fade_len;  /* frames mixed so far / total fade length */

static pl_state_t state = PL_STOPPED;
static uint32_t q_index, q_count;   /* position in the queue / songs in it */
static char cur_path[LIB_PATH_LEN];
static uint32_t serial;             /* +1 on every song change; the UI watches it */
static uint32_t clip_end_ms;
static bool clip_mode, clip_done;
static uint32_t pending_rate;       /* sample rate to switch to before the next write */
static uint32_t history[HISTORY];   /* queue indices played before, newest last */
static uint32_t hist_len;
static uint32_t last_resume_save;
static uint32_t rng = 0x12345678U;  /* shuffle random state */
static int32_t mix[MP3S_MAX_OUT_FRAMES * 2U];   /* crossfade output */

/* Small fast random number generator (xorshift) for shuffle. */
static uint32_t rnd(void)
{
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}

void player_init(void)
{
  rng ^= HAL_GetTick() * 2654435761U;    /* boot time varies a little: different shuffle each time */
  memset(dec, 0, sizeof(dec));
}

static void close_all(void)
{
  mp3s_close(&dec[0]);
  mp3s_close(&dec[1]);
  stage[0].len = stage[0].pos = 0;
  stage[1].len = stage[1].pos = 0;
  fading = false;
}

/* Open queue entry `index` in decoder slot 0 or 1. */
static bool open_into(int slot, uint32_t index, char *path_out)
{
  char path[LIB_PATH_LEN];
  if (!queue_get(index, path))
  {
    return false;
  }
  stage[slot].len = stage[slot].pos = 0;
  if (!mp3s_open(&dec[slot], path, g_settings.audio_32bit))
  {
    return false;
  }
  if (path_out)
  {
    strcpy(path_out, path);
  }
  return true;
}

/* Stop whatever is playing and start queue entry `index` at pos_ms. */
static bool begin_track(uint32_t index, uint32_t pos_ms)
{
  close_all();
  cur = 0;
  /* Skip over files that fail to open instead of stopping the queue. */
  for (uint32_t tries = 0; tries < q_count; tries++)
  {
    if (open_into(cur, index, cur_path))
    {
      q_index = index;
      if (pos_ms)
      {
        mp3s_seek(&dec[cur], pos_ms);
      }
      pending_rate = dec[cur].out_rate;
      serial++;
      return true;
    }
    index = (index + 1U) % q_count;
  }
  return false;
}

/* Which song comes next: random (never the same one) in shuffle, else the
 * next one. At the end of the queue: wrap if Repeat is on or the user
 * pressed Next, otherwise UINT32_MAX = stop. */
static uint32_t next_index(bool user)
{
  if (g_settings.shuffle && q_count > 1U)
  {
    uint32_t n;
    do
    {
      n = rnd() % q_count;
    } while (n == q_index);
    return n;
  }
  if (q_index + 1U < q_count)
  {
    return q_index + 1U;
  }
  return (g_settings.repeat || user) ? 0U : UINT32_MAX;
}

static void push_history(uint32_t i)
{
  if (hist_len == HISTORY)
  {
    memmove(history, history + 1, (HISTORY - 1U) * sizeof(history[0]));
    hist_len--;
  }
  history[hist_len++] = i;
}

/* Play the queue (queue.idx, filled by library.c) from entry `index`. */
bool player_start(uint32_t index, uint32_t pos_ms)
{
  clip_mode = false;
  q_count = queue_count();
  hist_len = 0;
  if (q_count == 0U)
  {
    state = PL_STOPPED;
    return false;
  }
  if (index >= q_count)
  {
    index = 0;
    pos_ms = 0;
  }
  audio_out_flush();
  audio_out_hold(false);
  if (!begin_track(index, pos_ms))
  {
    state = PL_STOPPED;
    return false;
  }
  if (audio_out_suspended())
  {
    audio_out_resume();
  }
  state = PL_PLAYING;
  return true;
}

bool player_play_clip(const char *path, uint32_t start_ms, uint32_t end_ms)
{
  if (!queue_from_single(path) || !player_start(0, start_ms))
  {
    return false;
  }
  clip_mode = true;
  clip_done = false;
  clip_end_ms = end_ms;
  return true;
}

bool player_clip_done(void)
{
  return clip_done;
}

void player_stop(void)
{
  close_all();
  audio_out_flush();
  state = PL_STOPPED;
}

void player_pause(void)
{
  if (state == PL_PLAYING)
  {
    state = PL_PAUSED;
    audio_out_hold(true);           /* silence now; queued audio waits for resume */
    player_save_resume();
  }
}

void player_toggle(void)
{
  if (state == PL_PLAYING)
  {
    player_pause();
  }
  else if (state == PL_PAUSED)
  {
    if (audio_out_suspended())
    {
      audio_out_resume();
    }
    audio_out_hold(false);
    state = PL_PLAYING;
  }
}

void player_next(void)
{
  if (state == PL_STOPPED || clip_mode)
  {
    return;
  }
  uint32_t n = next_index(true);
  push_history(q_index);
  audio_out_flush();
  if (!begin_track(n, 0))
  {
    state = PL_STOPPED;
  }
}

/* Like a CD player: more than 3 s into the song restarts it; otherwise go
 * to the previous song (in shuffle, the one actually played before). */
void player_prev(void)
{
  if (state == PL_STOPPED || clip_mode)
  {
    return;
  }
  pl_info_t i;
  player_info(&i);
  uint32_t n = q_index;
  if (i.pos_ms < 3000U)
  {
    if (g_settings.shuffle && hist_len)
    {
      n = history[--hist_len];
    }
    else
    {
      n = (q_index == 0U) ? q_count - 1U : q_index - 1U;
    }
  }
  audio_out_flush();
  if (!begin_track(n, 0))
  {
    state = PL_STOPPED;
  }
}

/* Jump forwards / backwards (Left/Right held in Now Playing). */
void player_seek_rel(int32_t delta_ms)
{
  if (state == PL_STOPPED || !dec[cur].open)
  {
    return;
  }
  pl_info_t i;
  player_info(&i);
  int32_t t = (int32_t)i.pos_ms + delta_ms;
  if (t < 0)
  {
    t = 0;
  }
  audio_out_flush();
  fading = false;
  mp3s_close(&dec[cur ^ 1]);
  mp3s_seek(&dec[cur], (uint32_t)t);
  stage[cur].len = stage[cur].pos = 0;
}

void player_volume_step(int dir)
{
  int v = (int)audio_out_volume() + dir * (int)AUDIO_VOLUME_STEP;
  if (v < 0)
  {
    v = 0;
  }
  if (v > 100)
  {
    v = 100;
  }
  audio_out_set_volume((uint8_t)v);
  g_settings.volume = (uint8_t)v;
}

/* ---- decode loop -------------------------------------------------------------- */

/* Make sure decoder s has frames waiting; returns how many. 0 = song over. */
static uint32_t stage_fill(int s)
{
  if (stage[s].pos == stage[s].len && dec[s].open)
  {
    stage[s].len = mp3s_decode(&dec[s], stage[s].buf);
    stage[s].pos = 0;
  }
  return stage[s].len - stage[s].pos;
}

/* The current song ended: go to the next one, or stop. */
static void finish_track(void)
{
  if (clip_mode)
  {
    clip_done = true;
    state = PL_STOPPED;
    close_all();
    return;
  }
  uint32_t n = next_index(false);
  if (n == UINT32_MAX)
  {
    state = PL_STOPPED;
    close_all();
    player_save_resume();
    return;
  }
  push_history(q_index);
  if (!begin_track(n, 0))
  {
    state = PL_STOPPED;
  }
}

/* Crossfade on and within 3 s of the end? Open the next song in the other
 * decoder so the two can be mixed. Skipped for very short songs and when
 * the next song has a different sample rate. */
static void maybe_start_fade(void)
{
  if (!g_settings.crossfade || clip_mode || fading || dec[cur].duration_ms < 2U * CROSSFADE_MS)
  {
    return;
  }
  if (mp3s_position_ms(&dec[cur]) + CROSSFADE_MS < dec[cur].duration_ms)
  {
    return;
  }
  uint32_t n = next_index(false);
  int other = cur ^ 1;
  if (n == UINT32_MAX || !open_into(other, n, NULL))
  {
    return;
  }
  if (dec[other].out_rate != dec[cur].out_rate)
  {
    mp3s_close(&dec[other]);      /* rate change needs a clean restart instead */
    return;
  }
  fading = true;
  fade_done = 0;
  fade_len = CROSSFADE_MS * dec[cur].out_rate / 1000U;
  push_history(q_index);
  q_index = n;
  queue_get(n, cur_path);
}

/* Main loop: decode until the audio ring is full. */
void player_task(void)
{
  if (state != PL_PLAYING)
  {
    return;
  }

  /* A new sample rate is applied once the old track has fully played out. */
  if (pending_rate)
  {
    if (pending_rate != audio_out_rate())
    {
      if (audio_out_queued() > 0U)
      {
        return;
      }
      audio_out_set_rate(pending_rate);
    }
    pending_rate = 0;
  }

  while (audio_out_space() >= MP3S_MAX_OUT_FRAMES)
  {
    if (clip_mode && mp3s_position_ms(&dec[cur]) >= clip_end_ms)
    {
      finish_track();
      return;
    }
    maybe_start_fade();

    uint32_t a = stage_fill(cur);
    if (a == 0U)
    {
      if (fading)
      {
        /* Old track ended early: the incoming track simply takes over. */
        mp3s_close(&dec[cur]);
        cur ^= 1;
        fading = false;
        serial++;
        continue;
      }
      finish_track();
      return;
    }

    if (!fading)                    /* normal case: copy straight through */
    {
      audio_out_write(&stage[cur].buf[stage[cur].pos * 2U], a);
      stage[cur].pos += a;
      continue;
    }

    int o = cur ^ 1;
    uint32_t b = stage_fill(o);
    uint32_t n = (b < a) ? b : a;
    if (n == 0U)
    {
      audio_out_write(&stage[cur].buf[stage[cur].pos * 2U], a);
      stage[cur].pos += a;
      continue;
    }
    /* Crossfade: old song x fades out while new song y fades in.
     * g goes 0 -> 1 over the fade; output = x * (1 - g) + y * g. */
    const int32_t *x = &stage[cur].buf[stage[cur].pos * 2U];
    const int32_t *y = &stage[o].buf[stage[o].pos * 2U];
    for (uint32_t i = 0; i < n; i++)
    {
      float g = (float)(fade_done + i) / (float)fade_len;
      if (g > 1.0f)
      {
        g = 1.0f;
      }
      mix[2U * i]      = (int32_t)((float)x[2U * i] * (1.0f - g) + (float)y[2U * i] * g);
      mix[2U * i + 1U] = (int32_t)((float)x[2U * i + 1U] * (1.0f - g) + (float)y[2U * i + 1U] * g);
    }
    audio_out_write(mix, n);
    stage[cur].pos += n;
    stage[o].pos += n;
    fade_done += n;
    if (fade_done >= fade_len)
    {
      mp3s_close(&dec[cur]);
      cur = o;
      fading = false;
      serial++;
    }
  }

  if (HAL_GetTick() - last_resume_save > RESUME_SAVE_MS && !clip_mode)
  {
    player_save_resume();
  }
}

/* Snapshot for the UI: state, song, tags, position and length. The position
 * is what you hear now, i.e. the decoder position minus the audio still
 * waiting in the ring. */
void player_info(pl_info_t *i)
{
  memset(i, 0, sizeof(*i));
  i->state = state;
  i->index = q_index;
  i->count = q_count;
  i->track_serial = serial;
  strcpy(i->path, cur_path);
  if (state == PL_STOPPED && !dec[cur].open)
  {
    return;
  }
  const mp3s_t *d = &dec[fading ? (cur ^ 1) : cur];
  i->meta = &d->meta;
  i->dur_ms = d->duration_ms;
  i->rate = d->out_rate;
  uint32_t pos = mp3s_position_ms(d);
  uint32_t lag = d->out_rate ? (uint32_t)((uint64_t)audio_out_queued() * 1000U / d->out_rate) : 0U;
  i->pos_ms = (pos > lag) ? pos - lag : 0U;
}

/* Length of a song that isn't playing (mixtape clip picker), using the spare
 * decoder. Returns 0 if the spare is busy with a crossfade. */
uint32_t player_probe_duration(const char *path)
{
  int spare = cur ^ 1;
  if (fading || dec[spare].open)
  {
    return 0;
  }
  uint32_t d = 0;
  if (mp3s_open(&dec[spare], path, true))
  {
    d = dec[spare].duration_ms;
    mp3s_close(&dec[spare]);
  }
  return d;
}

/* Write the resume point (also every 30 s while playing, in case the
 * battery dies without a clean shutdown). */
void player_save_resume(void)
{
  resume_t r;
  pl_info_t i;
  last_resume_save = HAL_GetTick();
  if (clip_mode || q_count == 0U)
  {
    return;
  }
  player_info(&i);
  r.valid = true;
  r.queue_index = q_index;
  r.position_ms = i.pos_ms;
  resume_save(&r);
}
