/**
  ******************************************************************************
  * @file    gfx.c
  * @brief   Two-plane (black + red) framebuffer for the 104 x 212 e-paper.
  *
  *  The screen lives in RAM as two bitmaps, 1 bit per pixel, 13 bytes per
  *  row (104 / 8), most significant bit = leftmost pixel:
  *    gfx_k: 1 = black pixel      gfx_r: 1 = red pixel      neither = white
  *  ui.c draws a whole screen with these functions and then passes both
  *  planes to display_submit().
  ******************************************************************************
  */
#include "gfx.h"
#include <string.h>

uint8_t gfx_k[EPD_PLANE_BYTES];
uint8_t gfx_r[EPD_PLANE_BYTES];

/* Make the whole screen white. */
void gfx_clear(void)
{
  memset(gfx_k, 0, sizeof(gfx_k));
  memset(gfx_r, 0, sizeof(gfx_r));
}

/* Set one pixel. Everything else is built on this. Off-screen is ignored. */
void gfx_pixel(int x, int y, gfx_color_t c)
{
  if (x < 0 || y < 0 || x >= GFX_W || y >= GFX_H)
  {
    return;
  }
  uint32_t i = (uint32_t)y * EPD_ROW_BYTES + (uint32_t)x / 8U;   /* byte */
  uint8_t  b = (uint8_t)(0x80U >> (x & 7));                      /* bit in it */

  /* "Pink" doesn't exist on the panel: fake it with a red/white checkerboard. */
  if (c == GFX_PINK)
  {
    c = ((x ^ y) & 1) ? GFX_RED : GFX_WHITE;
  }
  gfx_k[i] &= (uint8_t)~b;         /* clear to white, then set the one colour */
  gfx_r[i] &= (uint8_t)~b;
  if (c == GFX_BLACK)
  {
    gfx_k[i] |= b;
  }
  else if (c == GFX_RED)
  {
    gfx_r[i] |= b;
  }
}

/* Draw the Figma wallpaper (a 1-bit pattern from ui_assets.c) in pink. */
void gfx_wallpaper(void)
{
  for (int y = 0; y < GFX_H; y++)
  {
    for (int x = 0; x < GFX_W; x++)
    {
      if (ui_wallpaper[y * 13 + x / 8] & (0x80U >> (x & 7)))
      {
        gfx_pixel(x, y, GFX_PINK);
      }
    }
  }
}

void gfx_fill(int x, int y, int w, int h, gfx_color_t c)
{
  for (int j = y; j < y + h; j++)
  {
    for (int i = x; i < x + w; i++)
    {
      gfx_pixel(i, j, c);
    }
  }
}

void gfx_hline(int x, int y, int w, gfx_color_t c)
{
  gfx_fill(x, y, w, 1, c);
}

void gfx_rect(int x, int y, int w, int h, gfx_color_t c)
{
  gfx_fill(x, y, w, 1, c);
  gfx_fill(x, y + h - 1, w, 1, c);
  gfx_fill(x, y, 1, h, c);
  gfx_fill(x + w - 1, y, 1, h, c);
}

/* Is pixel (x, y) set in one plane of an icon? A NULL plane means the icon
 * has no pixels of that colour. */
static bool icon_bit(const uint8_t *plane, const ui_icon_t *icon, int x, int y)
{
  int bpr = (icon->w + 7) / 8;
  return plane && (plane[y * bpr + x / 8] & (0x80U >> (x & 7)));
}

/* Draw an icon in its own colours; white pixels are transparent. */
void gfx_icon(int x, int y, const ui_icon_t *icon)
{
  for (int j = 0; j < icon->h; j++)
  {
    for (int i = 0; i < icon->w; i++)
    {
      if (icon_bit(icon->k, icon, i, j))
      {
        gfx_pixel(x + i, y + j, GFX_BLACK);
      }
      else if (icon_bit(icon->r, icon, i, j))
      {
        gfx_pixel(x + i, y + j, GFX_RED);
      }
    }
  }
}

/* Draw an icon all in colour c, e.g. white on a selected (black) row. */
void gfx_icon_mono(int x, int y, const ui_icon_t *icon, gfx_color_t c)
{
  for (int j = 0; j < icon->h; j++)
  {
    for (int i = 0; i < icon->w; i++)
    {
      if (icon_bit(icon->k, icon, i, j) || icon_bit(icon->r, icon, i, j))
      {
        gfx_pixel(x + i, y + j, c);
      }
    }
  }
}

/* ---- text: Tiny5 bitmap font, ASCII 32..126 only ---------------------------
 * FONT_S and FONT_M are two sizes of the font; FONT_L is FONT_S drawn 2x. */
static const ui_font_t *font_of(gfx_font_t f, int *scale)
{
  *scale = (f == FONT_L) ? 2 : 1;
  return (f == FONT_M) ? &font_medium : &font_small;
}

static char printable(char ch)
{
  return (ch < 32 || ch > 126) ? '?' : ch;
}

/* Width of one character in pixels (the font is proportional). */
static int advance(char ch, gfx_font_t f)
{
  int scale;
  const ui_font_t *fn = font_of(f, &scale);
  return fn->advance[printable(ch) - 32] * scale;
}

/* Draw one character with its top-left corner at (x, y); returns its width. */
static int glyph(int x, int y, char ch, gfx_font_t f, gfx_color_t c)
{
  int scale;
  const ui_font_t *fn = font_of(f, &scale);
  int bpr = fn->bytes_per_row;
  const uint8_t *rows = &fn->rows[(printable(ch) - 32) * fn->h * bpr];

  for (int j = 0; j < fn->h; j++)
  {
    for (int i = 0; i < bpr * 8; i++)
    {
      if (rows[j * bpr + i / 8] & (0x80U >> (i & 7)))
      {
        gfx_fill(x + i * scale, y + j * scale, scale, scale, c);
      }
    }
  }
  return advance(ch, f);
}

int gfx_font_height(gfx_font_t f)
{
  int scale;
  return font_of(f, &scale)->h * scale;
}

/* Draw a string; returns the x just after the last character. */
int gfx_text(int x, int y, const char *s, gfx_font_t f, gfx_color_t c)
{
  while (*s)
  {
    x += glyph(x, y, *s++, f, c);
  }
  return x;
}

int gfx_text_width(const char *s, gfx_font_t f)
{
  int w = 0;
  for (; *s; s++)
  {
    w += advance(*s, f);
  }
  return w;
}

/* Centre a string on the screen width (too long: cut with ".."). */
void gfx_text_centered(int y, const char *s, gfx_font_t f, gfx_color_t c)
{
  int w = gfx_text_width(s, f);
  if (w > GFX_W - 4)
  {
    gfx_text_clip(2, y, s, f, c, GFX_W - 4);
    return;
  }
  gfx_text((GFX_W - w) / 2, y, s, f, c);
}

/* Draw a string in at most max_w pixels; if it doesn't fit, end it in "..",
 * e.g. long song titles in a list. */
void gfx_text_clip(int x, int y, const char *s, gfx_font_t f, gfx_color_t c, int max_w)
{
  if (gfx_text_width(s, f) <= max_w)
  {
    gfx_text(x, y, s, f, c);
    return;
  }
  int limit = x + max_w - gfx_text_width("..", f);
  for (; *s; s++)
  {
    if (x + advance(*s, f) > limit)
    {
      break;
    }
    x += glyph(x, y, *s, f, c);
  }
  gfx_text(x, y, "..", f, c);
}
