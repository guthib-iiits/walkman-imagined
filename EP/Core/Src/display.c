/**
  ******************************************************************************
  * @file    display.c
  * @brief   Decides how each new frame reaches the e-paper (see display.h).
  *
  *  The UI draws whole frames into RAM and hands them over with
  *  display_submit(). This file works out the cheapest way to get them onto
  *  the glass:
  *
  *    DS_OFF    panel asleep (no power used, image stays)
  *    DS_IDLE   awake, nothing running
  *    DS_FAST   ~0.5 s black/white update of just the changed rectangle
  *    DS_COLOUR ~15 s full refresh, the only way to draw red
  *
  *  It keeps three copies of the screen: what the UI wants (tgt_*), what we
  *  believe is on the glass (glass_*), and the black/white version we are
  *  about to send (fast_k). Each image has two "planes" (bit maps): k = black
  *  pixels, r = red pixels.
  ******************************************************************************
  */
#include "display.h"
#include "epd.h"
#include "buttons.h"
#include "walkman_config.h"
#include <string.h>

typedef enum {
  DS_OFF = 0,          /* panel in deep sleep, glass unchanged */
  DS_IDLE,             /* awake, ready for the next update */
  DS_FAST,             /* fast black/white partial update running */
  DS_COLOUR            /* full colour refresh running (can be aborted) */
} ds_state_t;

#define BUSY_POLL_MS 10U     /* how often to check the panel's BUSY pin */

static ds_state_t state = DS_OFF;
static uint8_t tgt_k[EPD_PLANE_BYTES], tgt_r[EPD_PLANE_BYTES];      /* latest frame */
static uint8_t glass_k[EPD_PLANE_BYTES], glass_r[EPD_PLANE_BYTES];  /* what the glass shows */
static uint8_t fast_k[EPD_PLANE_BYTES];   /* black/white frame for the next fast update */
static bool glass_unknown = true;     /* boot, or a refresh was aborted */
static bool first_frame = true;       /* nothing drawn since power-on */
static bool have_frame;               /* the UI has submitted at least one frame */
static bool colour_requested;         /* someone asked for a colour pass */
static bool new_frame;                /* target changed since the last black/white diff */
static bool check_colour;             /* target changed since the last colour check */
static uint16_t fast_count;           /* fast updates since the last colour pass (ghosting) */
static uint32_t last_submit, last_done, last_poll;

void display_init(void)
{
  memset(glass_k, 0, sizeof(glass_k));
  memset(glass_r, 0, sizeof(glass_r));
  state = DS_IDLE;
}

/* The UI hands over a finished frame. Only copied here; display_task() does
 * the actual drawing. An identical frame is ignored. */
void display_submit(const uint8_t *k, const uint8_t *r)
{
  if (memcmp(k, tgt_k, sizeof(tgt_k)) == 0 && memcmp(r, tgt_r, sizeof(tgt_r)) == 0)
  {
    return;
  }
  memcpy(tgt_k, k, sizeof(tgt_k));
  memcpy(tgt_r, r, sizeof(tgt_r));
  last_submit = HAL_GetTick();
  have_frame = true;
  new_frame = true;
  check_colour = true;

  /* Never make the user wait out a 15 s colour pass. */
  if (state == DS_COLOUR)
  {
    epd_abort();
    glass_unknown = true;
    state = DS_IDLE;
  }
}

void display_request_colour(void)
{
  colour_requested = true;
}

/* Black/white view of the target: red already on the glass is left alone
 * (white in the KW data, so the fast waveform skips it); any other red shows
 * black until the colour pass. */
static void build_fast_frame(void)
{
  for (uint32_t i = 0; i < EPD_PLANE_BYTES; i++)
  {
    uint8_t keep_red = glass_unknown ? 0U : (uint8_t)(tgt_r[i] & glass_r[i]);
    fast_k[i] = (uint8_t)((tgt_k[i] | tgt_r[i]) & ~keep_red);
  }
}

/* Bounding box of changed bytes; false when nothing changed. */
static bool diff_box(uint8_t *xb0, uint8_t *xb1, uint16_t *y0, uint16_t *y1)
{
  bool any = false;
  uint8_t bx0 = EPD_ROW_BYTES, bx1 = 0;
  uint16_t r0 = EPD_HEIGHT, r1 = 0;
  for (uint16_t y = 0; y < EPD_HEIGHT; y++)
  {
    for (uint8_t x = 0; x < EPD_ROW_BYTES; x++)
    {
      uint32_t i = (uint32_t)y * EPD_ROW_BYTES + x;
      if (fast_k[i] != glass_k[i])
      {
        any = true;
        bx0 = (x < bx0) ? x : bx0;
        bx1 = (x > bx1) ? x : bx1;
        r0 = (y < r0) ? y : r0;
        r1 = (y > r1) ? y : r1;
      }
    }
  }
  *xb0 = bx0;
  *xb1 = bx1;
  *y0 = r0;
  *y1 = r1;
  return any;
}

/* Send the full frame (black + red) with the panel's slow factory waveform.
 * Afterwards the glass matches the target exactly and all ghosting is gone. */
static void start_colour(void)
{
  epd_colour_start(tgt_k, tgt_r);
  memcpy(glass_k, tgt_k, sizeof(glass_k));
  memcpy(glass_r, tgt_r, sizeof(glass_r));
  glass_unknown = false;
  first_frame = false;
  colour_requested = false;
  fast_count = 0;
  state = DS_COLOUR;
}

/* Called from the main loop. Never blocks: it starts an update and returns,
 * then checks back until the panel says it is done. Priority order:
 *   1. wait for a running update to finish
 *   2. send changed pixels as a fast update
 *   3. once the user is idle, run a colour pass if red or ghosting needs it
 *   4. put the panel to sleep after DISPLAY_SLEEP_MS with nothing to do */
void display_task(void)
{
  uint32_t now = HAL_GetTick();

  /* 1. An update is in progress: poll BUSY (no more than every 10 ms). */
  if (state == DS_FAST || state == DS_COLOUR)
  {
    if (now - last_poll < BUSY_POLL_MS)
    {
      return;
    }
    last_poll = now;
    if (epd_busy())
    {
      return;
    }
    if (state == DS_FAST)
    {
      epd_fast_finish();
    }
    state = DS_IDLE;
    last_done = now;
  }

  /* Boot: the glass holds whatever was there at power-off, so start in colour. */
  if (first_frame)
  {
    if (have_frame)
    {
      start_colour();
    }
    return;
  }

  /* 2. Anything different in black/white? Send just that rectangle. Only
   *    worth checking after a new frame or an aborted refresh. */
  if (new_frame || glass_unknown)
  {
    new_frame = false;
    build_fast_frame();
    uint8_t xb0, xb1;
    uint16_t y0, y1;
    if (glass_unknown)
    {
      /* After an abort every pixel is suspect: drive the whole panel once. */
      for (uint32_t i = 0; i < EPD_PLANE_BYTES; i++)
      {
        glass_k[i] = (uint8_t)~fast_k[i];
      }
      memset(glass_r, 0, sizeof(glass_r));       /* red may be half-drawn: show it black */
      colour_requested = true;                   /* and redo the colour pass later */
      glass_unknown = false;
    }
    if (diff_box(&xb0, &xb1, &y0, &y1))
    {
      epd_fast_start(glass_k, fast_k, xb0, xb1, y0, y1);
      memcpy(glass_k, fast_k, sizeof(glass_k));
      fast_count++;
      state = DS_FAST;
      last_poll = now;
      return;
    }
  }

  /* 3. Black/white is up to date. Is a colour pass due? Only when nobody has
   *    touched a button for DISPLAY_COLOUR_DELAY_MS, so it never gets in the way. */
  bool quiet = (now - last_submit) >= DISPLAY_COLOUR_DELAY_MS &&
               (now - buttons_last_activity()) >= DISPLAY_COLOUR_DELAY_MS && !buttons_any_down();
  if (quiet && check_colour)
  {
    /* Red, or black hidden under the fast frame's red-as-black, still differs? */
    check_colour = false;
    if (memcmp(glass_r, tgt_r, sizeof(glass_r)) != 0 || memcmp(glass_k, tgt_k, sizeof(glass_k)) != 0)
    {
      colour_requested = true;
    }
  }
  if (quiet && (colour_requested || fast_count >= DISPLAY_CLEAN_EVERY))
  {
    start_colour();
    return;
  }

  /* 4. Nothing to do for a while: power the panel's charge pump down. */
  if (state == DS_IDLE && (now - last_done) >= DISPLAY_SLEEP_MS)
  {
    epd_sleep();
    state = DS_OFF;
  }
}

bool display_busy(void)
{
  return state == DS_FAST || state == DS_COLOUR;
}

bool display_colour_running(void)
{
  return state == DS_COLOUR;
}

/* true while black/white changes are still waiting to reach the glass. Used
 * by blocking messages ("Shutting down") to wait until they are visible. */
bool display_fast_pending(void)
{
  if (state == DS_FAST || first_frame)
  {
    return true;
  }
  build_fast_frame();
  return glass_unknown || memcmp(fast_k, glass_k, sizeof(fast_k)) != 0;
}
