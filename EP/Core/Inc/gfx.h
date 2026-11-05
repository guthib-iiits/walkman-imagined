/**
  ******************************************************************************
  * @file    gfx.h
  * @brief   Two-plane (black + red) framebuffer for the 104 x 212 e-paper.
  ******************************************************************************
  */
#ifndef WM_GFX_H
#define WM_GFX_H

#include <stdint.h>
#include <stdbool.h>
#include "epd.h"
#include "ui_assets.h"

typedef enum {
  GFX_WHITE = 0,
  GFX_BLACK,
  GFX_RED,
  GFX_PINK                 /* red at 50 % checkerboard: the Figma light pink */
} gfx_color_t;

#define GFX_W  ((int)EPD_WIDTH)
#define GFX_H  ((int)EPD_HEIGHT)

/* Text styles used by the Figma screens */
typedef enum {
  FONT_S = 0,      /* Tiny5 8 px: status bar, small labels */
  FONT_M,          /* Tiny5 12 px: list rows, track info */
  FONT_L           /* Tiny5 8 px at 2x: screen titles */
} gfx_font_t;

extern uint8_t gfx_k[EPD_PLANE_BYTES];
extern uint8_t gfx_r[EPD_PLANE_BYTES];

void gfx_clear(void);
void gfx_wallpaper(void);
void gfx_pixel(int x, int y, gfx_color_t c);
void gfx_fill(int x, int y, int w, int h, gfx_color_t c);
void gfx_rect(int x, int y, int w, int h, gfx_color_t c);
void gfx_hline(int x, int y, int w, gfx_color_t c);
void gfx_icon(int x, int y, const ui_icon_t *icon);
void gfx_icon_mono(int x, int y, const ui_icon_t *icon, gfx_color_t c);

/* Returns the x just past the last glyph. */
int  gfx_text(int x, int y, const char *s, gfx_font_t f, gfx_color_t c);
int  gfx_text_width(const char *s, gfx_font_t f);
int  gfx_font_height(gfx_font_t f);
void gfx_text_centered(int y, const char *s, gfx_font_t f, gfx_color_t c);
/* Draws at most max_w pixels, ending with ".." when the text is cut. */
void gfx_text_clip(int x, int y, const char *s, gfx_font_t f, gfx_color_t c, int max_w);

#endif /* WM_GFX_H */
