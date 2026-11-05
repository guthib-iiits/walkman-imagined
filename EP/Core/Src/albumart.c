/**
  ******************************************************************************
  * @file    albumart.c
  * @brief   Album art: embedded ID3 JPEG or cover.jpg / folder.jpg, decoded with
  *          TJpgDec and dithered to the panel's black / white / red.
  *
  *  Steps: find a JPEG -> decode it (TJpgDec hands us 8x8..16x16 pixel
  *  blocks) -> crop the centre square and scale it to 104 x 104 into `rgb`
  *  -> dither to 3 colours into art_k / art_r -> albumart_draw() copies
  *  those onto the screen. Done once per song, not on every redraw.
  ******************************************************************************
  */
#include "albumart.h"
#include "gfx.h"
#include "library.h"
#include "player.h"
#include "tjpgd.h"
#include "ff.h"
#include <string.h>

typedef struct {
  FIL f;
  uint32_t left;            /* bytes remaining in the JPEG */
  uint32_t crop_x, crop_y, side;   /* centre square of the (scaled) image */
} src_t;

static uint8_t rgb[ART_SIZE * ART_SIZE * 3U];       /* 104x104 colour image */
static uint8_t art_k[ART_SIZE * ART_SIZE / 8U];     /* result: black pixels */
static uint8_t art_r[ART_SIZE * ART_SIZE / 8U];     /* result: red pixels */
static uint8_t pool[8192];                          /* TJpgDec working memory */
static bool valid;                                  /* art for the current song is ready */

/* TJpgDec asks for more JPEG bytes through this (buf == NULL means skip). */
static size_t in_fn(JDEC *jd, uint8_t *buf, size_t n)
{
  src_t *s = jd->device;
  UINT br = 0;
  player_task();            /* decoding a big cover takes longer than the audio buffer */
  if (n > s->left)
  {
    n = s->left;
  }
  if (buf)
  {
    f_read(&s->f, buf, (UINT)n, &br);
  }
  else
  {
    f_lseek(&s->f, f_tell(&s->f) + n);
    br = (UINT)n;
  }
  s->left -= br;
  return br;
}

/* Nearest-neighbour crop + scale of each decoded block into the 104 x 104 buffer. */
static int out_fn(JDEC *jd, void *bitmap, JRECT *r)
{
  src_t *s = jd->device;
  const uint8_t *p = bitmap;
  for (int y = r->top; y <= r->bottom; y++)
  {
    for (int x = r->left; x <= r->right; x++, p += 3)
    {
      int cx = x - (int)s->crop_x, cy = y - (int)s->crop_y;
      if (cx < 0 || cy < 0 || cx >= (int)s->side || cy >= (int)s->side)
      {
        continue;
      }
      uint32_t dx = (uint32_t)cx * ART_SIZE / s->side;
      uint32_t dy = (uint32_t)cy * ART_SIZE / s->side;
      memcpy(&rgb[(dy * ART_SIZE + dx) * 3U], p, 3);
    }
  }
  return 1;
}

/* Floyd-Steinberg to the three inks the panel has.
 * Each pixel becomes the closest of white / black / red, and the colour error
 * is pushed onto the neighbours (7/16 right, 3/16 down-left, 5/16 down,
 * 1/16 down-right), so areas of in-between colour turn into patterns of
 * dots that look right from a distance. Only two rows of error are kept. */
static void dither(void)
{
  static const int16_t pal[3][3] = { { 255, 255, 255 }, { 0, 0, 0 }, { 220, 30, 40 } };
  static int16_t err[2][ART_SIZE + 2U][3];
  memset(err, 0, sizeof(err));
  memset(art_k, 0, sizeof(art_k));
  memset(art_r, 0, sizeof(art_r));

  for (uint32_t y = 0; y < ART_SIZE; y++)
  {
    int16_t (*cur)[3] = err[y & 1U];
    int16_t (*nxt)[3] = err[(y + 1U) & 1U];
    memset(nxt, 0, sizeof(err[0]));
    for (uint32_t x = 0; x < ART_SIZE; x++)
    {
      int16_t c[3];
      for (int k = 0; k < 3; k++)
      {
        int v = rgb[(y * ART_SIZE + x) * 3U + (uint32_t)k] + cur[x + 1U][k] / 16;
        c[k] = (int16_t)(v < 0 ? 0 : v > 255 ? 255 : v);
      }
      int best = 0;
      int32_t bd = INT32_MAX;
      for (int i = 0; i < 3; i++)
      {
        int32_t d = 0;
        for (int k = 0; k < 3; k++)
        {
          int32_t e = c[k] - pal[i][k];
          d += e * e;
        }
        if (d < bd)
        {
          bd = d;
          best = i;
        }
      }
      uint32_t bit = y * ART_SIZE + x;
      if (best == 1)
      {
        art_k[bit / 8U] |= (uint8_t)(0x80U >> (bit & 7U));
      }
      else if (best == 2)
      {
        art_r[bit / 8U] |= (uint8_t)(0x80U >> (bit & 7U));
      }
      for (int k = 0; k < 3; k++)
      {
        int16_t e = (int16_t)(c[k] - pal[best][k]);
        cur[x + 2U][k] = (int16_t)(cur[x + 2U][k] + e * 7);
        nxt[x][k]      = (int16_t)(nxt[x][k] + e * 3);
        nxt[x + 1U][k] = (int16_t)(nxt[x + 1U][k] + e * 5);
        nxt[x + 2U][k] = (int16_t)(nxt[x + 2U][k] + e);
      }
    }
  }
}

/* Decode a JPEG that starts `offset` bytes into `path` and is `size` bytes
 * long (size 0 = the whole file), then dither it. */
static bool decode(const char *path, uint32_t offset, uint32_t size)
{
  static src_t s;
  JDEC jd;

  if (f_open(&s.f, path, FA_READ) != FR_OK)
  {
    return false;
  }
  if (size == 0U)
  {
    size = (uint32_t)f_size(&s.f);
  }
  f_lseek(&s.f, offset);
  s.left = size;

  bool ok = false;
  if (jd_prepare(&jd, in_fn, pool, sizeof(pool), &s) == JDR_OK)
  {
    /* Largest TJpgDec scale (1/1 .. 1/8) that still covers 104 px. */
    uint8_t scale = 0;
    while (scale < 3U && (uint32_t)(jd.width >> (scale + 1U)) >= ART_SIZE &&
           (uint32_t)(jd.height >> (scale + 1U)) >= ART_SIZE)
    {
      scale++;
    }
    uint32_t w = jd.width >> scale, h = jd.height >> scale;
    s.side = (w < h) ? w : h;
    s.crop_x = (w - s.side) / 2U;
    s.crop_y = (h - s.side) / 2U;
    memset(rgb, 0xFF, sizeof(rgb));
    ok = jd_decomp(&jd, out_fn, scale) == JDR_OK;
  }
  f_close(&s.f);
  if (ok)
  {
    dither();
  }
  return ok;
}

/* Find art for a song: first the picture embedded in the MP3's ID3 tag
 * (offset/size from mp3_stream.c, 0 if none), then a cover file in the
 * song's folder. */
bool albumart_load(const char *track_path, uint32_t offset, uint32_t size)
{
  static const char *const names[] = { "cover.jpg", "folder.jpg", "Cover.jpg", "Folder.jpg" };
  char dir[LIB_PATH_LEN], p[LIB_PATH_LEN];

  valid = false;
  if (offset && decode(track_path, offset, size))
  {
    valid = true;
    return true;
  }
  strncpy(dir, track_path, sizeof(dir) - 1);
  dir[sizeof(dir) - 1] = '\0';
  char *slash = strrchr(dir, '/');
  if (!slash)
  {
    return false;
  }
  *slash = '\0';
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
  {
    lib_join(p, dir, names[i]);
    if (decode(p, 0, 0))
    {
      valid = true;
      return true;
    }
  }
  return false;
}

bool albumart_valid(void)
{
  return valid;
}

void albumart_draw(int x, int y)
{
  for (uint32_t j = 0; j < ART_SIZE; j++)
  {
    for (uint32_t i = 0; i < ART_SIZE; i++)
    {
      uint32_t bit = j * ART_SIZE + i;
      uint8_t m = (uint8_t)(0x80U >> (bit & 7U));
      gfx_color_t c = (art_k[bit / 8U] & m) ? GFX_BLACK : (art_r[bit / 8U] & m) ? GFX_RED : GFX_WHITE;
      gfx_pixel(x + (int)i, y + (int)j, c);
    }
  }
}
