/**
  ******************************************************************************
  * @file    ui.c
  * @brief   Screens from the Figma "EP/Walkman" board and their navigation.
  *
  *  Buttons (Walkman style):
  *    Lists      Up/Down move, Right/Centre open, Left back, hold Centre = add to
  *               Favourites / playlist
  *    Playing    Centre play/pause, Left/Right prev/next (hold = seek),
  *               Up/Down volume, hold Centre = main menu
  *    Anywhere   Power click = main menu, Power hold = shut down
  *
  *  Every change renders the whole frame into RAM and hands it to display.c,
  *  which refreshes only the changed rectangle with the fast black/white
  *  waveform. Anything that moves with the buttons (selection, cursors, scroll
  *  bar, progress) is drawn black so it never waits for a colour pass; red is
  *  kept for things that stay put (wallpaper, icons, custom text).
  *
  *  How this file is organised: every screen has a draw_xxx() that paints it
  *  into the framebuffer (gfx.c) and an input_xxx() that reacts to buttons.
  *  render() and ui_input() at the bottom pick the right pair for the screen
  *  on top of the stack. Input handlers never draw; they change state and
  *  call ui_invalidate(), and ui_task() redraws once things settle.
  *
  *  Screens form a stack: go() opens a screen on top, back() returns to the
  *  one below (Left button), home() goes straight back to the main menu.
  ******************************************************************************
  */
#include "ui.h"
#include "gfx.h"
#include "display.h"
#include "albumart.h"
#include "library.h"
#include "player.h"
#include "mixtape.h"
#include "settings.h"
#include "storage.h"
#include "rtc_clock.h"
#include "npm1300.h"
#include "audio_out.h"
#include "app.h"
#include "walkman_config.h"
#include <stdio.h>
#include <string.h>

/* Every screen in the UI. */
typedef enum {
  SCR_MENU = 0,          /* main menu: 6 icons */
  SCR_BROWSE,            /* Folders: files and folders */
  SCR_PLAYLISTS,         /* list of .m3u playlists */
  SCR_PLAYLIST_SONGS,    /* songs in one playlist */
  SCR_ADD_TO,            /* pick a playlist to add a song to (Centre hold) */
  SCR_NOW,               /* Now Playing: art, title, progress */
  SCR_SETTINGS,
  SCR_SET_TIME,
  SCR_EDIT_TEXT,         /* Settings -> Custom Text */
  SCR_MIX_HOME,          /* MixTapes: pick a song / list recordings */
  SCR_MIX_RANGE,         /* choose start and end of the clip */
  SCR_MIX_REC,           /* recording in progress */
  SCR_MIX_SAVE,          /* save or discard the recording */
  SCR_CONFIRM,           /* "Are you sure?" (format, shut down, restart) */
  SCR_MESSAGE,           /* two-line message, any button closes it */
  SCR_USB_OFFER,         /* cable plugged in: "USB drive mode?" */
  SCR_USB,               /* USB drive mode active */
  SCR_ABOUT,
  SCR_COUNT
} screen_t;

/* What the confirm screen is asking about. */
typedef enum {
  CONFIRM_FORMAT_SD,
  CONFIRM_FORMAT_EMMC,
  CONFIRM_SHUTDOWN,
  CONFIRM_RESTART
} confirm_t;

/* ---- state ---------------------------------------------------------------------- */

#define STACK_DEPTH 8

static screen_t stack[STACK_DEPTH];     /* stack[depth] is the screen shown */
static int depth;
static uint16_t sel[SCR_COUNT], top[SCR_COUNT];   /* per screen: selected row, first visible row */

static lib_list_t list;                 /* shared by browse / playlists / songs */
static bool browse_pick;                /* Folders opened to pick a mixtape song */
static char pick_path[LIB_PATH_LEN];
static char add_song[LIB_PATH_LEN];     /* song waiting for "add to playlist" */
static char cur_playlist[LIB_PATH_LEN];

static uint32_t mix_start_ms, mix_end_ms, mix_dur_ms;   /* clip being set up */
static uint8_t mix_field;               /* 0 = start, 1 = end */
static bool mix_ready;                  /* a song has been picked for the mixtape */

static uint8_t time_hh, time_mm, time_field;   /* Set Time editor; field 0 = hours, 1 = minutes */
static char edit_buf[24];               /* Custom Text editor */
static uint8_t edit_pos;                /* cursor position in edit_buf */

static confirm_t confirm_what;
static char msg1[32], msg2[32];         /* message screen text */

static bool dirty = true;               /* screen needs redrawing */
static uint32_t last_input, last_draw, last_batt;
static uint8_t last_minute = 0xFF;      /* redraw the clock when the minute changes */
static uint32_t art_serial = UINT32_MAX;   /* song the album art was loaded for */
static npm1300_status_t batt;           /* last battery reading, for the status bar */

static screen_t cur(void) { return stack[depth]; }
void ui_invalidate(void) { dirty = true; }
bool ui_refreshing(void) { return display_busy(); }

/* Open screen s on top of the current one, cursor on the first row. */
static void go(screen_t s)
{
  if (depth < STACK_DEPTH - 1)
  {
    depth++;
  }
  stack[depth] = s;
  sel[s] = 0;
  top[s] = 0;
  ui_invalidate();
}

static void back(void)
{
  if (depth > 0)
  {
    depth--;
  }
  ui_invalidate();
}

static void home(void)
{
  depth = 0;
  stack[0] = SCR_MENU;
  browse_pick = false;
  ui_invalidate();
}

/* Show a two-line message on top of the current screen. */
static void message(const char *a, const char *b)
{
  snprintf(msg1, sizeof(msg1), "%s", a);
  snprintf(msg2, sizeof(msg2), "%s", b ? b : "");
  go(SCR_MESSAGE);
}

/* 125000 ms -> "2:05" */
static void fmt_time(char *out, size_t n, uint32_t ms)
{
  uint32_t s = ms / 1000U;
  snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60U), (unsigned long)(s % 60U));
}

/* ---- common drawing ---------------------------------------------------------------- */

/* White, plus the pink wallpaper unless turned off (or High Contrast /
 * Low Power is on). */
static void background(void)
{
  gfx_clear();
  if (g_settings.wallpaper && !g_settings.high_contrast && !g_settings.low_power)
  {
    gfx_wallpaper();
  }
}

/* Top 13 pixels of every screen: repeat/shuffle, play state, clock, battery. */
static void status_bar(void)
{
  pl_info_t pi;
  player_info(&pi);
  gfx_fill(0, 0, GFX_W, 13, GFX_WHITE);

  if (g_settings.repeat || g_settings.shuffle)
  {
    gfx_icon(3, 5, &icon_st_repeat);
  }
  if (pi.state == PL_PLAYING)
  {
    gfx_icon(16, 4, &icon_st_playpause);
  }
  else if (pi.state == PL_PAUSED)
  {
    gfx_fill(16, 4, 2, 7, GFX_BLACK);
    gfx_fill(20, 4, 2, 7, GFX_BLACK);
  }

  if (g_settings.show_time && rtc_clock_valid())
  {
    uint8_t h, m, s;
    char t[8];
    rtc_clock_get(&h, &m, &s);
    snprintf(t, sizeof(t), "%02u:%02u", h, m);
    gfx_text(36, 3, t, FONT_S, GFX_BLACK);
  }

  /* Battery: outline, nub and four segments, as in the Figma status bar */
  int bx = 74, by = 3;
  gfx_rect(bx + 2, by, 25, 9, GFX_BLACK);
  gfx_fill(bx, by + 2, 2, 5, GFX_BLACK);
  int segs = batt.present ? (batt.percent + 24) / 25 : 0;
  for (int i = 0; i < 4; i++)
  {
    if (4 - i <= segs)
    {
      gfx_fill(bx + 4 + i * 6, by + 2, 5, 5, g_settings.high_contrast ? GFX_BLACK : GFX_RED);
    }
  }
  if (batt.charging)
  {
    gfx_text(bx - 6, by + 1, "+", FONT_S, GFX_BLACK);
  }
}

static void title(const char *t, int y)
{
  gfx_text_centered(y, t, FONT_L, GFX_BLACK);
}

/* A scrolling list: rows at FONT_M with an icon column. */
typedef void (*row_fn)(uint16_t i, int y, bool selected);

/* Shared by every list screen. Scrolls so the selected row is visible, draws
 * the selected row as a black bar, and calls fn to draw each row's content.
 * count = total rows, rows = how many fit, pitch = row height in pixels. */
static void draw_list(screen_t s, uint16_t count, int y0, int pitch, int rows, row_fn fn)
{
  if (sel[s] >= count)
  {
    sel[s] = count ? (uint16_t)(count - 1U) : 0U;
  }
  if (sel[s] < top[s])
  {
    top[s] = sel[s];
  }
  if (sel[s] >= top[s] + rows)
  {
    top[s] = (uint16_t)(sel[s] - rows + 1);
  }
  for (int r = 0; r < rows && top[s] + r < count; r++)
  {
    uint16_t i = (uint16_t)(top[s] + r);
    int y = y0 + r * pitch;
    bool on = (i == sel[s]);
    if (on)
    {
      gfx_fill(0, y - 2, GFX_W, pitch - 1, GFX_BLACK);
    }
    fn(i, y, on);
  }
  if (count > (uint16_t)rows)
  {
    /* scroll position on the right edge */
    int h = rows * pitch;
    int bar = h * rows / count;
    if (bar < 4)
    {
      bar = 4;
    }
    int pos = (h - bar) * sel[s] / (count - 1);
    gfx_fill(GFX_W - 2, y0 - 2 + pos, 2, bar, GFX_BLACK);
  }
  if (count == 0U)
  {
    gfx_text_centered(y0 + 20, "Empty", FONT_M, GFX_BLACK);
  }
}

/* Text colour for a row: white on the black selection bar, else black. */
static gfx_color_t ink(bool selected)
{
  return selected ? GFX_WHITE : GFX_BLACK;
}

/* ---- main menu --------------------------------------------------------------------- */

/* The 2 x 3 icon grid, positions from the Figma frame. */
typedef struct {
  const char *label;
  const ui_icon_t *icon;
  int x, y;
} menu_item_t;

static const menu_item_t menu_items[6] = {
  { "Folders",     &icon_folder,   6,  40 },
  { "Shuffle",     &icon_shuffle,  40, 40 },
  { "Playlists",   &icon_playlist, 74, 40 },
  { "Now Playing", &icon_disc,     6,  96 },
  { "Settings",    &icon_gear,     40, 96 },
  { "MixTapes",    &icon_cassette, 74, 96 },
};

static void draw_menu(void)
{
  pl_info_t pi;
  player_info(&pi);
  title("Main Menu", 17);
  for (int i = 0; i < 6; i++)
  {
    const menu_item_t *m = &menu_items[i];
    bool on = (sel[SCR_MENU] == i);
    gfx_fill(m->x, m->y, 24, 24, GFX_WHITE);
    gfx_rect(m->x, m->y, 24, 24, GFX_BLACK);
    if (on)
    {
      gfx_rect(m->x - 1, m->y - 1, 26, 26, GFX_BLACK);
      gfx_rect(m->x - 2, m->y - 2, 28, 28, GFX_BLACK);
    }
    gfx_icon(m->x + (24 - m->icon->w) / 2, m->y + (24 - m->icon->h) / 2, m->icon);
  }
  if (pi.meta)
  {
    gfx_text_clip(4, 135, pi.meta->title, FONT_M, GFX_BLACK, 96);
    gfx_text_clip(4, 150, pi.meta->album[0] ? pi.meta->album : pi.meta->artist, FONT_M, GFX_BLACK, 96);
  }
  gfx_text_centered(189, menu_items[sel[SCR_MENU]].label, FONT_L, GFX_BLACK);
}

static void start_library_shuffle(void)
{
  g_settings.shuffle = true;
  if (queue_from_library() && player_start(HAL_GetTick() % queue_count(), 0))   /* random first track */
  {
    go(SCR_NOW);
  }
  else
  {
    message("No music found", "Copy MP3s over USB");
  }
}

static void open_browse(const char *path)
{
  lib_list_dir(&list, path);
  go(SCR_BROWSE);
}

/* Main menu: the arrows move around the 3 x 2 grid (wrapping at the edges),
 * Centre opens the selected item. */
static void input_menu(const btn_event_t *ev)
{
  uint16_t *s = &sel[SCR_MENU];
  if (ev->type != BTN_EV_CLICK)
  {
    return;
  }
  switch (ev->id)
  {
    case BTN_LEFT:  *s = (uint16_t)((*s % 3U == 0U) ? *s + 2U : *s - 1U); break;
    case BTN_RIGHT: *s = (uint16_t)((*s % 3U == 2U) ? *s - 2U : *s + 1U); break;
    case BTN_UP:
    case BTN_DOWN:  *s = (uint16_t)((*s + 3U) % 6U); break;
    case BTN_CENTER:
      switch (*s)
      {
        case 0: browse_pick = false; open_browse(""); return;
        case 1: start_library_shuffle(); return;
        case 2: playlists_list(&list); go(SCR_PLAYLISTS); return;
        case 3: go(SCR_NOW); return;
        case 4: go(SCR_SETTINGS); return;
        case 5: go(SCR_MIX_HOME); return;
      }
      break;
    default:
      return;
  }
  ui_invalidate();
}

/* ---- folder browser ---------------------------------------------------------------- */

/* The Folders screen. `list` holds the current folder's contents; Right /
 * Centre goes into a folder or plays a song, Left goes up a level. The same
 * screen is reused to pick a song for a mixtape (browse_pick). */

/* One row: folder or note icon, then the name without ".mp3". */
static void row_browse(uint16_t i, int y, bool on)
{
  const lib_entry_t *e = &list.e[i];
  if (on)
  {
    gfx_icon_mono(4, y, e->is_dir ? &icon_folder_small : &icon_song_small, GFX_WHITE);
  }
  else
  {
    gfx_icon(4, y, e->is_dir ? &icon_folder_small : &icon_song_small);
  }
  char name[LIB_NAME_LEN];
  strcpy(name, e->name);
  if (!e->is_dir)
  {
    char *dot = strrchr(name, '.');
    if (dot)
    {
      *dot = '\0';
    }
  }
  gfx_text_clip(20, y - 1, name, FONT_M, ink(on), GFX_W - 24);
}

/* Title = current folder's name. */
static const char *browse_title(void)
{
  if (list.path[0] == '\0')
  {
    return browse_pick ? "Pick Song" : "Files";
  }
  const char *slash = strrchr(list.path, '/');
  return slash ? slash + 1 : "SD Card";
}

static void draw_browse(void)
{
  title(browse_title(), 17);
  draw_list(SCR_BROWSE, list.count, 41, 15, 11, row_browse);
}

/* Left: go to the parent folder. From "1:/Music" or "0:" go to the top
 * level (Music / SD Card); from the top level leave the screen. */
static void browse_up(void)
{
  if (list.path[0] == '\0')
  {
    back();
    return;
  }
  char *slash = strrchr(list.path, '/');
  bool at_root = (strcmp(list.path, EMMC_DRIVE "/Music") == 0) || (strcmp(list.path, SD_DRIVE) == 0);
  char parent[LIB_PATH_LEN];
  if (at_root || !slash)
  {
    parent[0] = '\0';
  }
  else
  {
    size_t n = (size_t)(slash - list.path);
    memcpy(parent, list.path, n);
    parent[n] = '\0';
    if (parent[n - 1] == ':')
    {
      strcat(parent, "/");
    }
  }
  lib_list_dir(&list, parent);
  sel[SCR_BROWSE] = top[SCR_BROWSE] = 0;
  ui_invalidate();
}

/* Right / Centre on the selected row: enter a folder, pick a mixtape song,
 * or queue the whole folder and start playing at this song. */
static void browse_open(void)
{
  if (list.count == 0U)
  {
    return;
  }
  uint16_t i = sel[SCR_BROWSE];
  const lib_entry_t *e = &list.e[i];
  char p[LIB_PATH_LEN];

  if (list.path[0] == '\0')
  {
    strcpy(p, strcmp(e->name, "Music") == 0 ? EMMC_DRIVE "/Music" : SD_DRIVE);
  }
  else
  {
    lib_join(p, list.path, e->name);
  }
  if (e->is_dir)
  {
    lib_list_dir(&list, p);
    sel[SCR_BROWSE] = top[SCR_BROWSE] = 0;
    ui_invalidate();
    return;
  }
  if (browse_pick)
  {
    /* Mixtape: song picked, now choose the clip */
    strcpy(pick_path, p);
    mix_dur_ms = player_probe_duration(p);
    if (mix_dur_ms == 0U)
    {
      mix_dur_ms = 10U * 60U * 1000U;
    }
    mix_start_ms = 0;
    mix_end_ms = (mix_dur_ms > 30000U) ? 30000U : mix_dur_ms;
    mix_field = 0;
    browse_pick = false;
    back();
    go(SCR_MIX_RANGE);
    return;
  }
  uint32_t qi;
  if (queue_from_dir(&list, i, &qi) && player_start(qi, 0))
  {
    go(SCR_NOW);
  }
  else
  {
    message("Can't play", e->name);
  }
}

/* Up/Down in any list, wrapping round at the ends. Holding repeats, for
 * fast scrolling. */
static void input_list_nav(screen_t s, uint16_t count, const btn_event_t *ev)
{
  if (ev->type != BTN_EV_CLICK && ev->type != BTN_EV_REPEAT)
  {
    return;
  }
  if (ev->id == BTN_UP && count)
  {
    sel[s] = (sel[s] == 0U) ? (uint16_t)(count - 1U) : (uint16_t)(sel[s] - 1U);
    ui_invalidate();
  }
  else if (ev->id == BTN_DOWN && count)
  {
    sel[s] = (uint16_t)((sel[s] + 1U) % count);
    ui_invalidate();
  }
}

static void input_browse(const btn_event_t *ev)
{
  input_list_nav(SCR_BROWSE, list.count, ev);
  if (ev->type == BTN_EV_CLICK)
  {
    if (ev->id == BTN_LEFT)
    {
      browse_up();
    }
    else if (ev->id == BTN_RIGHT || ev->id == BTN_CENTER)
    {
      browse_open();
    }
  }
  /* Centre held on a song: "add to playlist". */
  else if (ev->type == BTN_EV_HOLD && ev->id == BTN_CENTER && list.count && !browse_pick &&
           !list.e[sel[SCR_BROWSE]].is_dir && list.path[0])
  {
    lib_join(add_song, list.path, list.e[sel[SCR_BROWSE]].name);
    playlists_list(&list);
    go(SCR_ADD_TO);
  }
}

/* ---- playlists --------------------------------------------------------------------- */

/* Row 0 is "Create new"; the rest are .m3u files, Favourites first. */
static void row_playlists(uint16_t i, int y, bool on)
{
  if (i == 0U)
  {
    gfx_text(20, y - 1, "+ Create new", FONT_M, on ? GFX_WHITE : GFX_RED);
    return;
  }
  const lib_entry_t *e = &list.e[i - 1U];
  char name[LIB_NAME_LEN];
  strcpy(name, e->name);
  char *dot = strrchr(name, '.');
  if (dot)
  {
    *dot = '\0';
  }
  if (strcmp(name, "Favourites") == 0)
  {
    if (on)
    {
      gfx_icon_mono(3, y - 1, &icon_heart, GFX_WHITE);
    }
    else
    {
      gfx_icon(3, y - 1, &icon_heart);
    }
  }
  gfx_text_clip(20, y - 1, name, FONT_M, ink(on), GFX_W - 24);
}

static void draw_playlists(screen_t s)
{
  title(s == SCR_ADD_TO ? "Add to" : "Playlists", 17);
  draw_list(s, (uint16_t)(list.count + 1U), 41, 15, 11, row_playlists);
}

/* Used twice: as the Playlists screen (open a playlist) and as "Add to"
 * (put add_song into the chosen playlist). */
static void input_playlists(screen_t s, const btn_event_t *ev)
{
  input_list_nav(s, (uint16_t)(list.count + 1U), ev);
  if (ev->type != BTN_EV_CLICK)
  {
    return;
  }
  if (ev->id == BTN_LEFT)
  {
    back();
    return;
  }
  if (ev->id != BTN_RIGHT && ev->id != BTN_CENTER)
  {
    return;
  }
  uint16_t i = sel[s];
  char p[LIB_PATH_LEN];
  if (i == 0U)
  {
    if (!playlist_create(p))
    {
      message("Can't create", "playlist");
      return;
    }
    if (s == SCR_ADD_TO)
    {
      playlist_add(p, add_song);
      back();
      message("Added to", strrchr(p, '/') + 1);
      return;
    }
    playlists_list(&list);
    ui_invalidate();
    return;
  }
  lib_join(p, PLAYLIST_DIR, list.e[i - 1U].name);
  if (s == SCR_ADD_TO)
  {
    playlist_add(p, add_song);
    back();
    message("Added to", list.e[i - 1U].name);
    return;
  }
  strcpy(cur_playlist, p);
  playlist_songs(&list, p);
  go(SCR_PLAYLIST_SONGS);
}

static void row_songs(uint16_t i, int y, bool on)
{
  char name[LIB_NAME_LEN];
  strcpy(name, list.e[i].name);
  char *dot = strrchr(name, '.');
  if (dot)
  {
    *dot = '\0';
  }
  if (on)
  {
    gfx_icon_mono(4, y, &icon_song_small, GFX_WHITE);
  }
  else
  {
    gfx_icon(4, y, &icon_song_small);
  }
  gfx_text_clip(20, y - 1, name, FONT_M, ink(on), GFX_W - 24);
}

static void draw_playlist_songs(void)
{
  const char *n = strrchr(cur_playlist, '/');
  char t[LIB_NAME_LEN];
  snprintf(t, sizeof(t), "%s", n ? n + 1 : cur_playlist);
  char *dot = strrchr(t, '.');
  if (dot)
  {
    *dot = '\0';
  }
  title(t, 17);
  draw_list(SCR_PLAYLIST_SONGS, list.count, 41, 15, 11, row_songs);
}

static void input_playlist_songs(const btn_event_t *ev)
{
  input_list_nav(SCR_PLAYLIST_SONGS, list.count, ev);
  if (ev->type != BTN_EV_CLICK)
  {
    return;
  }
  if (ev->id == BTN_LEFT)
  {
    playlists_list(&list);
    back();
  }
  else if ((ev->id == BTN_RIGHT || ev->id == BTN_CENTER) && list.count)
  {
    if (queue_from_playlist(cur_playlist) && player_start(sel[SCR_PLAYLIST_SONGS], 0))
    {
      go(SCR_NOW);
    }
    else
    {
      message("Can't play", "playlist");
    }
  }
}

/* ---- now playing ------------------------------------------------------------------- */

/* Layout (y): status bar 0-12, album art 24-127, title / artist / album
 * 131-170, progress bar 175, times 184, custom text 196. */
static void draw_now(void)
{
  pl_info_t pi;
  player_info(&pi);

  if (albumart_valid())
  {
    albumart_draw(0, 24);
  }
  else
  {
    gfx_fill(0, 24, 104, 104, g_settings.high_contrast ? GFX_WHITE : GFX_PINK);
    gfx_rect(0, 24, 104, 104, GFX_BLACK);
    gfx_fill(18, 58, 68, 36, GFX_WHITE);
    gfx_text_centered(60, "Album", FONT_L, GFX_BLACK);
    gfx_text_centered(77, "Art", FONT_L, GFX_BLACK);
  }

  gfx_fill(0, 129, GFX_W, 83, GFX_WHITE);
  if (!pi.meta)
  {
    gfx_text_centered(140, "Nothing playing", FONT_M, GFX_BLACK);
    gfx_text_centered(160, "Pick a song", FONT_M, GFX_BLACK);
    return;
  }
  gfx_text_clip(3, 131, pi.meta->title, FONT_M, GFX_BLACK, 98);
  gfx_text_clip(3, 145, pi.meta->artist[0] ? pi.meta->artist : "Unknown artist", FONT_M, GFX_BLACK, 98);
  gfx_text_clip(3, 159, pi.meta->album, FONT_M, GFX_BLACK, 98);

  if (g_settings.progress_bar && pi.dur_ms)
  {
    char a[12], b[12];
    gfx_rect(3, 175, 98, 6, GFX_BLACK);
    int w = (int)((uint64_t)pi.pos_ms * 94U / pi.dur_ms);
    gfx_fill(5, 177, w > 94 ? 94 : w, 2, GFX_BLACK);
    fmt_time(a, sizeof(a), pi.pos_ms);
    fmt_time(b, sizeof(b), pi.dur_ms);
    gfx_text(3, 184, a, FONT_S, GFX_BLACK);
    gfx_text(GFX_W - 3 - gfx_text_width(b, FONT_S), 184, b, FONT_S, GFX_BLACK);
  }
  gfx_text_centered(196, g_settings.custom_text, FONT_M, g_settings.high_contrast ? GFX_BLACK : GFX_RED);
}

/* Click: play/pause, previous/next, volume. Hold Left/Right: seek 5 s per
 * repeat. Hold Centre: back to the main menu (music keeps playing). */
static void input_now(const btn_event_t *ev)
{
  switch (ev->type)
  {
    case BTN_EV_CLICK:
      switch (ev->id)
      {
        case BTN_CENTER: player_toggle(); ui_invalidate(); break;
        case BTN_LEFT:   player_prev(); break;
        case BTN_RIGHT:  player_next(); break;
        case BTN_UP:     player_volume_step(+1); break;
        case BTN_DOWN:   player_volume_step(-1); break;
        default: break;
      }
      break;
    case BTN_EV_HOLD:
      if (ev->id == BTN_CENTER)
      {
        settings_save();
        home();
        break;
      }
      __attribute__((fallthrough));   /* first seek / volume step */
    case BTN_EV_REPEAT:
      if (ev->id == BTN_LEFT)
      {
        player_seek_rel(-5000);
      }
      else if (ev->id == BTN_RIGHT)
      {
        player_seek_rel(+5000);
      }
      else if (ev->id == BTN_UP)
      {
        player_volume_step(+1);
      }
      else if (ev->id == BTN_DOWN)
      {
        player_volume_step(-1);
      }
      break;
    case BTN_EV_RELEASE:
      if (ev->id == BTN_LEFT || ev->id == BTN_RIGHT)
      {
        ui_invalidate();                 /* show where the seek landed */
      }
      else if (ev->id == BTN_UP || ev->id == BTN_DOWN)
      {
        settings_save();
      }
      break;
    default:
      break;
  }
}

/* ---- settings ---------------------------------------------------------------------- */

/* Settings rows, in screen order. set_names[] must stay in the same order. */
typedef enum {
  SET_AUDIO, SET_PROGRESS, SET_WALLPAPER, SET_HYSTERESIS, SET_CROSSFADE, SET_SLEEP,
  SET_LOW_POWER, SET_CONTRAST, SET_SHOW_TIME, SET_SET_TIME, SET_REPEAT, SET_CUSTOM_TEXT,
  SET_USB, SET_RESCAN, SET_FORMAT_SD, SET_ABOUT, SET_RESTART, SET_SHUTDOWN, SET_COUNT
} set_item_t;

static const char *const set_names[SET_COUNT] = {
  "Audio Quality", "Progress Bar", "Wallpaper", "Hysteresis", "Crossfade", "Sleep Timer",
  "Low Power", "High Contrast", "Time", "Set Time", "Repeat", "Custom Text",
  "USB Drive", "Rescan Music", "Format SD", "About", "Restart", "Shutdown"
};

/* The setting an on/off row toggles; NULL for rows that open something. */
static bool *set_bool(set_item_t i)
{
  switch (i)
  {
    case SET_PROGRESS:   return &g_settings.progress_bar;
    case SET_WALLPAPER:  return &g_settings.wallpaper;
    case SET_HYSTERESIS: return &g_settings.hysteresis;
    case SET_CROSSFADE:  return &g_settings.crossfade;
    case SET_LOW_POWER:  return &g_settings.low_power;
    case SET_CONTRAST:   return &g_settings.high_contrast;
    case SET_SHOW_TIME:  return &g_settings.show_time;
    case SET_REPEAT:     return &g_settings.repeat;
    default:             return NULL;
  }
}

/* Setting name on the left; on the right a toggle switch, a value
 * ("32bit", "30m") or ">" for rows that open another screen. */
static void row_settings(uint16_t i, int y, bool on)
{
  gfx_text_clip(4, y - 1, set_names[i], FONT_M, ink(on), 68);
  bool *b = set_bool((set_item_t)i);
  char v[8] = "";
  if (b)
  {
    gfx_icon(78, y, *b ? &icon_toggle_on : &icon_toggle_off);
    if (on && !*b)
    {
      gfx_fill(82, y + 2, 9, 3, GFX_WHITE);
    }
    return;
  }
  if (i == SET_AUDIO)
  {
    strcpy(v, g_settings.audio_32bit ? "32bit" : "16bit");
  }
  else if (i == SET_SLEEP)
  {
    if (g_settings.sleep_min)
    {
      snprintf(v, sizeof(v), "%um", g_settings.sleep_min);
    }
    else
    {
      strcpy(v, "Off");
    }
  }
  else
  {
    strcpy(v, ">");
  }
  gfx_text(GFX_W - 4 - gfx_text_width(v, FONT_S), y + 1, v, FONT_S, ink(on));
}

static void draw_settings(void)
{
  title("Settings", 17);
  draw_list(SCR_SETTINGS, SET_COUNT, 42, 17, 10, row_settings);
}

/* Centre / Right on a row: flip a toggle, step through values, or open the
 * row's screen. Leaving with Left saves the settings file. */
static void input_settings(const btn_event_t *ev)
{
  input_list_nav(SCR_SETTINGS, SET_COUNT, ev);
  if (ev->type != BTN_EV_CLICK)
  {
    return;
  }
  if (ev->id == BTN_LEFT)
  {
    settings_save();
    back();
    return;
  }
  if (ev->id != BTN_RIGHT && ev->id != BTN_CENTER)
  {
    return;
  }
  set_item_t i = (set_item_t)sel[SCR_SETTINGS];
  bool *b = set_bool(i);
  if (b)
  {
    *b = !*b;
    if (i == SET_LOW_POWER)
    {
      app_apply_low_power();
    }
    ui_invalidate();
    return;
  }
  switch (i)
  {
    case SET_AUDIO:
      g_settings.audio_32bit = !g_settings.audio_32bit;  /* applies from the next track */
      break;
    case SET_SLEEP:                /* each press: Off -> 15 -> 30 -> 60 -> 90 min -> Off */
    {
      static const uint8_t steps[] = { 0, 15, 30, 60, 90 };
      uint8_t n = 0;
      for (uint8_t k = 0; k < sizeof(steps); k++)
      {
        if (steps[k] == g_settings.sleep_min)
        {
          n = (uint8_t)((k + 1U) % sizeof(steps));
        }
      }
      g_settings.sleep_min = steps[n];
      app_sleep_timer_restart();
      break;
    }
    case SET_SET_TIME:
    {
      uint8_t s;
      rtc_clock_get(&time_hh, &time_mm, &s);
      time_field = 0;
      go(SCR_SET_TIME);
      return;
    }
    case SET_CUSTOM_TEXT:
      memset(edit_buf, 0, sizeof(edit_buf));
      strncpy(edit_buf, g_settings.custom_text, sizeof(edit_buf) - 1);
      edit_pos = 0;
      go(SCR_EDIT_TEXT);
      return;
    case SET_USB:
      app_usb_enter();
      go(SCR_USB);
      return;
    case SET_RESCAN:
    {
      char n[24];
      ui_message_blocking("Scanning", "music...");
      snprintf(n, sizeof(n), "%lu songs", (unsigned long)library_rebuild());
      message("Library ready", n);
      return;
    }
    case SET_FORMAT_SD:
      confirm_what = CONFIRM_FORMAT_SD;
      go(SCR_CONFIRM);
      return;
    case SET_ABOUT:
      go(SCR_ABOUT);
      return;
    case SET_RESTART:
      confirm_what = CONFIRM_RESTART;
      go(SCR_CONFIRM);
      return;
    case SET_SHUTDOWN:
      confirm_what = CONFIRM_SHUTDOWN;
      go(SCR_CONFIRM);
      return;
    default:
      break;
  }
  ui_invalidate();
}

/* ---- set time / custom text ---------------------------------------------------------- */

/* Set Time: Left/Right pick hours or minutes (underlined), Up/Down change
 * them, Centre saves to the RTC. */
static void draw_set_time(void)
{
  char t[8];
  title("Set Time", 17);
  snprintf(t, sizeof(t), "%02u", time_hh);
  gfx_text(16, 80, t, FONT_L, GFX_BLACK);
  gfx_text(48, 80, ":", FONT_L, GFX_BLACK);
  snprintf(t, sizeof(t), "%02u", time_mm);
  gfx_text(60, 80, t, FONT_L, GFX_BLACK);
  gfx_fill(time_field ? 60 : 16, 99, 26, 3, GFX_BLACK);
  gfx_text_centered(150, "Up/Down change", FONT_M, GFX_BLACK);
  gfx_text_centered(166, "Centre to save", FONT_M, GFX_BLACK);
}

static void input_set_time(const btn_event_t *ev)
{
  if (ev->type != BTN_EV_CLICK && ev->type != BTN_EV_REPEAT)
  {
    return;
  }
  switch (ev->id)
  {
    case BTN_UP:
      if (time_field) { time_mm = (uint8_t)((time_mm + 1U) % 60U); }
      else            { time_hh = (uint8_t)((time_hh + 1U) % 24U); }
      break;
    case BTN_DOWN:
      if (time_field) { time_mm = (uint8_t)((time_mm + 59U) % 60U); }
      else            { time_hh = (uint8_t)((time_hh + 23U) % 24U); }
      break;
    case BTN_LEFT:
      if (time_field == 0U)
      {
        back();
        return;
      }
      time_field = 0;
      break;
    case BTN_RIGHT:
      time_field = 1;
      break;
    case BTN_CENTER:
      rtc_clock_set(time_hh, time_mm);
      back();
      return;
    default:
      return;
  }
  ui_invalidate();
}

/* Custom Text: no keyboard, so each position is "spun" through these
 * characters with Up/Down, like setting a combination lock. */
static const char charset[] =" ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!?.-'&<3";

static void draw_edit_text(void)
{
  title("Custom Text", 17);
  gfx_rect(2, 70, 100, 20, GFX_BLACK);
  gfx_text(5, 74, edit_buf, FONT_M, GFX_BLACK);
  char before[24];
  memcpy(before, edit_buf, edit_pos);
  before[edit_pos] = '\0';
  int x = 5 + gfx_text_width(before, FONT_M);
  gfx_fill(x, 87, 6, 2, GFX_BLACK);
  gfx_text_centered(120, "Up/Down letter", FONT_M, GFX_BLACK);
  gfx_text_centered(136, "Left/Right move", FONT_M, GFX_BLACK);
  gfx_text_centered(152, "Centre to save", FONT_M, GFX_BLACK);
}

static void input_edit_text(const btn_event_t *ev)
{
  if (ev->type != BTN_EV_CLICK && ev->type != BTN_EV_REPEAT)
  {
    return;
  }
  char *c = &edit_buf[edit_pos];
  const char *p = strchr(charset, *c ? *c : ' ');
  int idx = p ? (int)(p - charset) : 0;
  int n = (int)strlen(charset);
  switch (ev->id)
  {
    case BTN_UP:    *c = charset[(idx + 1) % n]; break;
    case BTN_DOWN:  *c = charset[(idx + n - 1) % n]; break;
    case BTN_RIGHT:
      if (edit_pos < sizeof(edit_buf) - 2U)
      {
        if (*c == '\0')
        {
          *c = ' ';
        }
        edit_pos++;
      }
      break;
    case BTN_LEFT:
      if (edit_pos == 0U)
      {
        back();
        return;
      }
      edit_pos--;
      break;
    case BTN_CENTER:
    {
      size_t len = strlen(edit_buf);
      while (len && edit_buf[len - 1] == ' ')
      {
        edit_buf[--len] = '\0';
      }
      strcpy(g_settings.custom_text, edit_buf);
      settings_save();
      back();
      return;
    }
    default:
      return;
  }
  ui_invalidate();
}

/* ---- mixtapes ---------------------------------------------------------------------- */

/* Flow: MixTapes home -> "New MixTape" -> pick a song (Folders screen) ->
 * Timeframe (start / end) -> back home -> "Record" -> Recording ->
 * Save point (save / discard). "My MixTapes" lists the saved WAVs. */

/* Home: 3 rows; "Record" only appears once a song and clip are chosen. */
static void draw_mix_home(void)
{
  title("MixTapes", 23);
  gfx_icon(8, 45, &icon_tape_big);

  static const char *const items[3] = { "My MixTapes", "New MixTape", "Record" };
  for (int i = 0; i < 3; i++)
  {
    int y = 108 + i * 17;
    bool on = sel[SCR_MIX_HOME] == i;
    if (i == 2 && !mix_ready)
    {
      continue;
    }
    if (on)
    {
      gfx_fill(0, y - 2, GFX_W, 16, GFX_BLACK);
    }
    if (i == 0)
    {
      on ? gfx_icon_mono(6, y, &icon_folder_small, GFX_WHITE) : gfx_icon(6, y, &icon_folder_small);
    }
    else if (i == 1)
    {
      on ? gfx_icon_mono(6, y - 1, &icon_disc, GFX_WHITE) : gfx_icon(6, y - 1, &icon_disc);
    }
    else
    {
      gfx_fill(7, y + 1, 9, 9, GFX_RED);
    }
    gfx_text(22, y - 1, items[i], FONT_M, ink(on));
  }
  if (mix_ready)
  {
    char a[12], b[12], t[28];
    const char *n = strrchr(pick_path, '/');
    gfx_text_clip(4, 162, n ? n + 1 : pick_path, FONT_S, GFX_BLACK, 96);
    fmt_time(a, sizeof(a), mix_start_ms);
    fmt_time(b, sizeof(b), mix_end_ms);
    snprintf(t, sizeof(t), "%s - %s", a, b);
    gfx_text(4, 172, t, FONT_S, GFX_BLACK);
  }
  gfx_icon(28, 182, &icon_rec_dot);
}

static void input_mix_home(const btn_event_t *ev)
{
  uint16_t count = mix_ready ? 3U : 2U;
  input_list_nav(SCR_MIX_HOME, count, ev);
  if (ev->type != BTN_EV_CLICK)
  {
    return;
  }
  if (ev->id == BTN_LEFT)
  {
    back();
    return;
  }
  if (ev->id != BTN_CENTER && ev->id != BTN_RIGHT)
  {
    return;
  }
  switch (sel[SCR_MIX_HOME])
  {
    case 0:
      if (!storage_sd_mounted())
      {
        storage_sd_rescan();
      }
      if (!storage_sd_mounted())
      {
        message("Insert SD card", "for MixTapes");
        return;
      }
      storage_mkdirs(MIXTAPE_DIR);
      browse_pick = false;
      open_browse(MIXTAPE_DIR);
      return;
    case 1:
      browse_pick = true;
      open_browse("");
      return;
    case 2:
      if (mixtape_begin(pick_path, mix_start_ms, mix_end_ms))
      {
        go(SCR_MIX_REC);
      }
      else
      {
        message("Can't record", mixtape_error());
      }
      return;
  }
}

/* Timeframe: choose the part of the song to record over. Up/Down move the
 * selected time by 1 s (10 s while held). */
static void draw_mix_range(void)
{
  char a[12], b[12];
  const char *n = strrchr(pick_path, '/');
  title("Timeframe", 17);
  gfx_text_clip(4, 42, n ? n + 1 : pick_path, FONT_M, GFX_BLACK, 96);
  fmt_time(a, sizeof(a), mix_start_ms);
  fmt_time(b, sizeof(b), mix_end_ms);
  gfx_text(6, 70, "Start", FONT_M, GFX_BLACK);
  gfx_text(56, 66, a, FONT_L, GFX_BLACK);
  gfx_text(6, 100, "End", FONT_M, GFX_BLACK);
  gfx_text(56, 96, b, FONT_L, GFX_BLACK);
  gfx_fill(54, mix_field ? 114 : 84, 48, 2, GFX_BLACK);
  gfx_text_centered(140, "Up/Down: 1s", FONT_M, GFX_BLACK);
  gfx_text_centered(155, "hold: 10s", FONT_M, GFX_BLACK);
  gfx_text_centered(172, "Centre: done", FONT_M, GFX_BLACK);
}

static void input_mix_range(const btn_event_t *ev)
{
  uint32_t *v = mix_field ? &mix_end_ms : &mix_start_ms;
  uint32_t step = (ev->type == BTN_EV_REPEAT || ev->type == BTN_EV_HOLD) ? 10000U : 1000U;
  if (ev->type == BTN_EV_RELEASE)
  {
    ui_invalidate();
    return;
  }
  if (ev->type != BTN_EV_CLICK && ev->type != BTN_EV_REPEAT && ev->type != BTN_EV_HOLD)
  {
    return;
  }
  switch (ev->id)
  {
    case BTN_UP:
      *v = (*v + step > mix_dur_ms) ? mix_dur_ms : *v + step;
      break;
    case BTN_DOWN:
      *v = (*v > step) ? *v - step : 0U;
      break;
    case BTN_LEFT:
      if (mix_field == 0U)
      {
        back();
        return;
      }
      mix_field = 0;
      break;
    case BTN_RIGHT:
      mix_field = 1;
      break;
    case BTN_CENTER:
      if (mix_end_ms <= mix_start_ms + 1000U)
      {
        message("End must be", "after start");
        return;
      }
      mix_ready = true;
      back();
      sel[SCR_MIX_HOME] = 2;
      ui_invalidate();
      return;
    default:
      return;
  }
  if (mix_start_ms >= mix_end_ms && mix_field == 0U)
  {
    mix_end_ms = (mix_start_ms + 30000U > mix_dur_ms) ? mix_dur_ms : mix_start_ms + 30000U;
  }
  if (ev->type == BTN_EV_CLICK)
  {
    ui_invalidate();
  }
}

/* Recording screen. It changes to Save point by itself (in ui_task) when
 * the clip ends; Centre stops early. */
static void draw_mix_rec(void)
{
  char a[12], b[24];
  title("Recording", 23);
  gfx_icon(28, 60, &icon_rec_dot);
  fmt_time(a, sizeof(a), mix_end_ms - mix_start_ms);
  snprintf(b, sizeof(b), "Clip %s", a);
  gfx_text_centered(120, b, FONT_M, GFX_BLACK);
  gfx_text_centered(140, "Sing along!", FONT_M, GFX_RED);
  gfx_text_centered(176, "Centre: stop", FONT_M, GFX_BLACK);
}

static void input_mix_rec(const btn_event_t *ev)
{
  if (ev->type == BTN_EV_CLICK && ev->id == BTN_CENTER)
  {
    mixtape_stop();
  }
}

static void draw_mix_save(void)
{
  char a[12];
  title("Save point", 17);
  gfx_icon(8, 40, &icon_tape_big);
  fmt_time(a, sizeof(a), mixtape_elapsed_ms());
  gfx_text_centered(98, a, FONT_L, GFX_BLACK);
  static const char *const items[2] = { "Save MixTape", "Discard" };
  for (int i = 0; i < 2; i++)
  {
    int y = 130 + i * 18;
    bool on = sel[SCR_MIX_SAVE] == i;
    if (on)
    {
      gfx_fill(0, y - 2, GFX_W, 16, GFX_BLACK);
    }
    gfx_text(10, y - 1, items[i], FONT_M, ink(on));
  }
}

static void input_mix_save(const btn_event_t *ev)
{
  input_list_nav(SCR_MIX_SAVE, 2, ev);
  if (ev->type != BTN_EV_CLICK || (ev->id != BTN_CENTER && ev->id != BTN_RIGHT))
  {
    return;
  }
  if (sel[SCR_MIX_SAVE] == 0U)
  {
    char n[24];
    depth -= 2;                              /* back to the MixTapes home */
    if (mixtape_save(n, sizeof(n)))
    {
      mix_ready = false;
      sel[SCR_MIX_HOME] = 0;
      message("Saved", n);
    }
    else
    {
      message("Save failed", "SD card?");
    }
  }
  else
  {
    mixtape_discard();
    depth -= 2;
    ui_invalidate();
  }
}

/* ---- confirm / message / usb / about ------------------------------------------------- */

/* "Are you sure?" for anything destructive. Needs a Centre *hold*, so a
 * stray click can't format a card. */
static void draw_confirm(void)
{
  static const char *const what[] = { "Format SD?", "Format eMMC?", "Shut down?", "Restart?" };
  title("Sure?", 17);
  gfx_text_centered(70, what[confirm_what], FONT_M, GFX_BLACK);
  if (confirm_what == CONFIRM_FORMAT_SD || confirm_what == CONFIRM_FORMAT_EMMC)
  {
    gfx_text_centered(90, "Erases everything", FONT_S, GFX_RED);
  }
  gfx_text_centered(140, "Hold Centre: yes", FONT_M, GFX_BLACK);
  gfx_text_centered(156, "Left: no", FONT_M, GFX_BLACK);
}

static void input_confirm(const btn_event_t *ev)
{
  if (ev->type == BTN_EV_CLICK && ev->id == BTN_LEFT)
  {
    back();
    return;
  }
  if (ev->type != BTN_EV_HOLD || ev->id != BTN_CENTER)
  {
    return;
  }
  switch (confirm_what)
  {
    case CONFIRM_FORMAT_SD:
      ui_message_blocking("Formatting", "SD card...");
      back();
      message(storage_format_sd() ? "SD formatted" : "Format failed", "");
      break;
    case CONFIRM_FORMAT_EMMC:
      ui_message_blocking("Formatting", "eMMC...");
      if (storage_format_emmc())
      {
        settings_save();
        home();
      }
      else
      {
        message("Format failed", "check eMMC");
      }
      break;
    case CONFIRM_SHUTDOWN:
      app_shutdown();
      break;
    case CONFIRM_RESTART:
      app_restart();
      break;
  }
}

static void draw_message(void)
{
  gfx_fill(4, 70, 96, 60, GFX_WHITE);
  gfx_rect(4, 70, 96, 60, GFX_BLACK);
  gfx_text_centered(84, msg1, FONT_M, GFX_BLACK);
  gfx_text_centered(102, msg2, FONT_M, GFX_RED);
}

static void draw_usb_offer(void)
{
  title("USB", 17);
  gfx_text_centered(60, "Computer found", FONT_M, GFX_BLACK);
  gfx_text_centered(100, "Centre: copy", FONT_M, GFX_BLACK);
  gfx_text_centered(116, "music", FONT_M, GFX_BLACK);
  gfx_text_centered(140, "Left: just", FONT_M, GFX_BLACK);
  gfx_text_centered(156, "charge", FONT_M, GFX_BLACK);
}

static void input_usb_offer(const btn_event_t *ev)
{
  if (ev->type != BTN_EV_CLICK)
  {
    return;
  }
  if (ev->id == BTN_CENTER)
  {
    app_usb_enter();
    stack[depth] = SCR_USB;
    ui_invalidate();
  }
  else if (ev->id == BTN_LEFT)
  {
    back();
  }
}

/* USB drive mode: the PC sees the eMMC. Centre (after ejecting on the PC)
 * hands it back and rescans the music. */
static void draw_usb(void)
{
  char t[24];
  title("USB Drive", 17);
  gfx_icon(41, 50, &icon_folder);
  snprintf(t, sizeof(t), "eMMC %lu MB", (unsigned long)storage_emmc_mb());
  gfx_text_centered(76, t, FONT_M, GFX_BLACK);
  gfx_text_centered(110, "Eject on the", FONT_M, GFX_BLACK);
  gfx_text_centered(126, "computer, then", FONT_M, GFX_BLACK);
  gfx_text_centered(142, "press Centre", FONT_M, GFX_RED);
}

static void input_usb(const btn_event_t *ev)
{
  if (ev->type == BTN_EV_CLICK && ev->id == BTN_CENTER)
  {
    ui_message_blocking("Updating", "library...");
    app_usb_exit();
    home();
  }
}

/* About: hardware status, handy when bringing up a new board. */
static void draw_about(void)
{
  char t[32];
  title("About", 17);
  snprintf(t, sizeof(t), "eMMC  %lu MB", (unsigned long)storage_emmc_mb());
  gfx_text(4, 44, t, FONT_M, GFX_BLACK);
  gfx_text(4, 60, storage_sd_mounted() ? "SD    ready" : "SD    none", FONT_M, GFX_BLACK);
  snprintf(t, sizeof(t), "Songs %lu", (unsigned long)library_count());
  gfx_text(4, 76, t, FONT_M, GFX_BLACK);
  if (batt.present)
  {
    snprintf(t, sizeof(t), "Batt  %u.%02uV %u%%", batt.vbat_mv / 1000U, (batt.vbat_mv % 1000U) / 10U, batt.percent);
  }
  else
  {
    strcpy(t, "PMIC  not found");
  }
  gfx_text(4, 92, t, FONT_M, GFX_BLACK);
  snprintf(t, sizeof(t), "DACs  %s", app_dacs_ok() ? "2x CS43131" : "not found");
  gfx_text(4, 108, t, FONT_M, GFX_BLACK);
  snprintf(t, sizeof(t), "Rate  %lu Hz", (unsigned long)audio_out_rate());
  gfx_text(4, 124, t, FONT_M, GFX_BLACK);
  snprintf(t, sizeof(t), "Vol   %u%%", audio_out_volume());
  gfx_text(4, 140, t, FONT_M, GFX_BLACK);
  gfx_text_centered(190, "EP Walkman v1.0", FONT_S, GFX_RED);
}

/* ---- render + refresh scheduling ----------------------------------------------------- */

/* Draw the current screen into the framebuffer. */
static void render(void)
{
  screen_t s = cur();
  background();
  status_bar();
  switch (s)
  {
    case SCR_MENU:           draw_menu(); break;
    case SCR_BROWSE:         draw_browse(); break;
    case SCR_PLAYLISTS:
    case SCR_ADD_TO:         draw_playlists(s); break;
    case SCR_PLAYLIST_SONGS: draw_playlist_songs(); break;
    case SCR_NOW:            draw_now(); break;
    case SCR_SETTINGS:       draw_settings(); break;
    case SCR_SET_TIME:       draw_set_time(); break;
    case SCR_EDIT_TEXT:      draw_edit_text(); break;
    case SCR_MIX_HOME:       draw_mix_home(); break;
    case SCR_MIX_RANGE:      draw_mix_range(); break;
    case SCR_MIX_REC:        draw_mix_rec(); break;
    case SCR_MIX_SAVE:       draw_mix_save(); break;
    case SCR_CONFIRM:        draw_confirm(); break;
    case SCR_MESSAGE:        draw_message(); break;
    case SCR_USB_OFFER:      draw_usb_offer(); break;
    case SCR_USB:            draw_usb(); break;
    case SCR_ABOUT:          draw_about(); break;
    default: break;
  }
}

/* Show a message and wait until it is actually on the glass (e.g. "Shutting
 * down" before power-off). Keeps the audio fed while waiting. */
void ui_message_blocking(const char *line1, const char *line2)
{
  background();
  status_bar();
  snprintf(msg1, sizeof(msg1), "%s", line1);
  snprintf(msg2, sizeof(msg2), "%s", line2 ? line2 : "");
  draw_message();
  display_submit(gfx_k, gfx_r);
  while (display_fast_pending())
  {
    display_task();
    player_task();
  }
  ui_invalidate();
}

/* First screen after boot, chosen by app_init(). The main menu is always at
 * the bottom of the stack, so Left gets back to it. */
void ui_init(ui_start_t start)
{
  depth = 0;
  stack[0] = SCR_MENU;
  npm1300_update(&batt);
  switch (start)
  {
    case UI_START_NOW_PLAYING:
      go(SCR_NOW);
      break;
    case UI_START_FORMAT_EMMC:
      confirm_what = CONFIRM_FORMAT_EMMC;
      go(SCR_CONFIRM);
      break;
    case UI_START_LOW_BATTERY:
      message("Battery low", "Please charge");
      break;
    default:
      break;
  }
  last_input = 0;
  ui_invalidate();
}

/* USB cable just plugged in: ask about drive mode (not while recording). */
void ui_offer_usb(void)
{
  if (cur() != SCR_USB && cur() != SCR_USB_OFFER && cur() != SCR_MIX_REC)
  {
    go(SCR_USB_OFFER);
  }
}

/* One button event from app_loop(). Power is handled the same everywhere;
 * every other button goes to the current screen's input_xxx(). */
void ui_input(const btn_event_t *ev)
{
  last_input = HAL_GetTick();

  if (ev->id == BTN_POWER)
  {
    if (ev->type == BTN_EV_HOLD)
    {
      app_shutdown();
    }
    else if (ev->type == BTN_EV_CLICK && cur() != SCR_USB && cur() != SCR_MIX_REC)
    {
      settings_save();
      home();
    }
    return;
  }

  screen_t s = cur();
  switch (s)
  {
    case SCR_MENU:           input_menu(ev); break;
    case SCR_BROWSE:         input_browse(ev); break;
    case SCR_PLAYLISTS:
    case SCR_ADD_TO:         input_playlists(s, ev); break;
    case SCR_PLAYLIST_SONGS: input_playlist_songs(ev); break;
    case SCR_NOW:            input_now(ev); break;
    case SCR_SETTINGS:       input_settings(ev); break;
    case SCR_SET_TIME:       input_set_time(ev); break;
    case SCR_EDIT_TEXT:      input_edit_text(ev); break;
    case SCR_MIX_HOME:       input_mix_home(ev); break;
    case SCR_MIX_RANGE:      input_mix_range(ev); break;
    case SCR_MIX_REC:        input_mix_rec(ev); break;
    case SCR_MIX_SAVE:       input_mix_save(ev); break;
    case SCR_CONFIRM:        input_confirm(ev); break;
    case SCR_USB_OFFER:      input_usb_offer(ev); break;
    case SCR_USB:            input_usb(ev); break;
    case SCR_MESSAGE:
    case SCR_ABOUT:
      if (ev->type == BTN_EV_CLICK)
      {
        back();
      }
      break;
    default:
      break;
  }
}

/* Main loop: notice things that change without a button press (song
 * ended, recording finished, clock ticked) and redraw when needed. */
void ui_task(void)
{
  uint32_t now = HAL_GetTick();
  pl_info_t pi;

  /* Mixtape recording finished on its own: offer the save point. */
  if (cur() == SCR_MIX_REC && mixtape_state() != MT_RECORDING)
  {
    if (mixtape_state() == MT_FINISHED)
    {
      stack[depth] = SCR_MIX_SAVE;
      sel[SCR_MIX_SAVE] = 0;
    }
    else
    {
      stack[depth] = SCR_MESSAGE;
      snprintf(msg1, sizeof(msg1), "Recording failed");
      snprintf(msg2, sizeof(msg2), "%s", mixtape_error());
    }
    ui_invalidate();
  }

  /* New track: fetch its art and redraw Now Playing. */
  player_info(&pi);
  if (pi.track_serial != art_serial && pi.meta)
  {
    art_serial = pi.track_serial;
    albumart_load(pi.path, pi.meta->art_offset, pi.meta->art_size);
    if (cur() == SCR_NOW || cur() == SCR_MENU)
    {
      ui_invalidate();
    }
  }

  /* Progress bar and clock are small black/white changes: cheap fast updates. */
  if (cur() == SCR_NOW && pi.state == PL_PLAYING && g_settings.progress_bar && !g_settings.low_power &&
      now - last_draw > UI_PROGRESS_PERIOD_MS)
  {
    dirty = true;
  }
  if (g_settings.show_time && rtc_clock_valid())
  {
    uint8_t h, m, s;
    rtc_clock_get(&h, &m, &s);
    if (m != last_minute)
    {
      last_minute = m;
      dirty = true;
    }
  }

  /* Frames are only handed over between fast refreshes, so a burst of key
   * presses collapses into one update of the latest state. */
  if (dirty && (now - last_input) >= UI_REDRAW_SETTLE_MS && (!display_busy() || display_colour_running()))
  {
    dirty = false;
    if (now - last_batt > 5000U || last_batt == 0U)
    {
      npm1300_update(&batt);
      last_batt = now;
    }
    render();
    display_submit(gfx_k, gfx_r);
    last_draw = now;
  }
}
