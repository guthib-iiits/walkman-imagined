/**
  ******************************************************************************
  * @file    library.c
  * @brief   Folder browsing, .m3u playlists, the shuffle-all index and the
  *          play queue.
  *
  *  RAM is too small to hold thousands of song paths, so lists of songs live
  *  in files on the eMMC as fixed-size records: every path takes exactly
  *  LIB_PATH_LEN bytes. Song number i is then at byte i * LIB_PATH_LEN, so
  *  any entry can be read straight away without scanning the file.
  *    queue.idx    the songs currently being played, in order
  *    library.idx  every song on the eMMC and SD card (for "shuffle all")
  *  Playlists are normal .m3u text files (one path per line) so they can be
  *  made on a PC.
  ******************************************************************************
  */
#include "library.h"
#include "storage.h"
#include "walkman_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char rec[LIB_PATH_LEN];      /* one record, zero-padded before writing */
static FILINFO fi;                  /* static: FILINFO is large for the stack */

/* Songs we can play: .mp3 and .wav (any case). */
bool lib_is_audio(const char *name)
{
  const char *dot = strrchr(name, '.');
  return dot && (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".wav") == 0);
}

/* out = dir + "/" + name, without doubling the slash. */
void lib_join(char *out, const char *dir, const char *name)
{
  size_t n = strlen(dir);
  if (n && dir[n - 1] == '/')
  {
    snprintf(out, LIB_PATH_LEN, "%s%s", dir, name);
  }
  else
  {
    snprintf(out, LIB_PATH_LEN, "%s/%s", dir, name);
  }
}

/* Sort order for lists: folders first, then A-Z ignoring case. */
static int cmp_entry(const void *a, const void *b)
{
  const lib_entry_t *x = a, *y = b;
  if (x->is_dir != y->is_dir)
  {
    return x->is_dir ? -1 : 1;
  }
  return strcasecmp(x->name, y->name);
}

/* Add one row to an on-screen list; past LIB_MAX_ENTRIES it is marked as cut short. */
static void add(lib_list_t *l, const char *name, bool dir)
{
  if (l->count >= LIB_MAX_ENTRIES)
  {
    l->truncated = true;
    return;
  }
  strncpy(l->e[l->count].name, name, LIB_NAME_LEN - 1);
  l->e[l->count].name[LIB_NAME_LEN - 1] = '\0';
  l->e[l->count].is_dir = dir;
  l->count++;
}

/* Fill a list for the Files screen: sub-folders and songs in `path`.
 * path "" is the top level, which shows "Music" (eMMC) and "SD Card". */
bool lib_list_dir(lib_list_t *l, const char *path)
{
  DIR d;
  l->count = 0;
  l->truncated = false;
  strncpy(l->path, path, LIB_PATH_LEN - 1);
  l->path[LIB_PATH_LEN - 1] = '\0';

  if (path[0] == '\0')
  {
    if (storage_emmc_mounted())
    {
      add(l, "Music", true);
    }
    if (storage_sd_mounted())
    {
      add(l, "SD Card", true);
    }
    return true;
  }
  if (f_opendir(&d, path) != FR_OK)
  {
    return false;
  }
  while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
  {
    if (fi.fname[0] == '.' || (fi.fattrib & (AM_HID | AM_SYS)))
    {
      continue;
    }
    bool dir = (fi.fattrib & AM_DIR) != 0;
    if (dir || lib_is_audio(fi.fname))
    {
      add(l, fi.fname, dir);
    }
  }
  f_closedir(&d);
  qsort(l->e, l->count, sizeof(l->e[0]), cmp_entry);
  return true;
}

/* ---- fixed-record files ----------------------------------------------------- */

/* Write one path as a full LIB_PATH_LEN record. */
static bool rec_append(FIL *f, const char *path)
{
  UINT bw;
  memset(rec, 0, sizeof(rec));
  strncpy(rec, path, sizeof(rec) - 1);
  return f_write(f, rec, sizeof(rec), &bw) == FR_OK && bw == sizeof(rec);
}

/* Number of records = file size / record size. */
static uint32_t rec_count(const char *file)
{
  return (f_stat(file, &fi) == FR_OK) ? (uint32_t)(fi.fsize / LIB_PATH_LEN) : 0U;
}

/* Read record i (seek straight to it). */
static bool rec_get(const char *file, uint32_t i, char *path)
{
  FIL f;
  UINT br = 0;
  bool ok = false;
  if (f_open(&f, file, FA_READ) == FR_OK)
  {
    if (f_lseek(&f, (FSIZE_t)i * LIB_PATH_LEN) == FR_OK &&
        f_read(&f, path, LIB_PATH_LEN, &br) == FR_OK && br == LIB_PATH_LEN)
    {
      path[LIB_PATH_LEN - 1] = '\0';
      ok = path[0] != '\0';
    }
    f_close(&f);
  }
  return ok;
}

/* ---- queue ------------------------------------------------------------------ */

/* The queue can be filled four ways: a folder, a playlist, the whole
 * library, or one song. Each rewrites queue.idx. */

/* Queue every song in a folder listing; *index = queue position of the song
 * the user picked (entry), so playback starts there. */
bool queue_from_dir(const lib_list_t *l, uint16_t entry, uint32_t *index)
{
  FIL f;
  char p[LIB_PATH_LEN];
  uint32_t n = 0;

  if (f_open(&f, QUEUE_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
  {
    return false;
  }
  *index = 0;
  for (uint16_t i = 0; i < l->count; i++)
  {
    if (l->e[i].is_dir)
    {
      continue;
    }
    if (i == entry)
    {
      *index = n;
    }
    lib_join(p, l->path, l->e[i].name);
    rec_append(&f, p);
    n++;
  }
  f_close(&f);
  return n > 0U;
}

/* Turn one .m3u line into a full path we can open:
 *   "1:/Music/a.mp3"  drive given: use as is
 *   "/Music/a.mp3"    same drive as the playlist
 *   "a.mp3"           relative to the playlist's folder
 * Windows backslashes become forward slashes. */
static void m3u_resolve(char *out, const char *m3u_path, const char *line)
{
  if (line[0] && line[1] == ':')
  {
    snprintf(out, LIB_PATH_LEN, "%s", line);
  }
  else if (line[0] == '/')
  {
    snprintf(out, LIB_PATH_LEN, "%.2s%s", m3u_path, line);
  }
  else
  {
    char dir[LIB_PATH_LEN];
    strncpy(dir, m3u_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash)
    {
      *slash = '\0';
    }
    lib_join(out, dir, line);
  }
  for (char *c = out; *c; c++)
  {
    if (*c == '\\')
    {
      *c = '/';
    }
  }
}

/* Next song path from an .m3u file, skipping blank lines and "#" comments. */
static bool m3u_next(FIL *f, const char *m3u_path, char *out)
{
  char line[LIB_PATH_LEN];
  while (f_gets(line, sizeof(line), f))
  {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
    {
      line[--n] = '\0';
    }
    if (n == 0U || line[0] == '#')
    {
      continue;
    }
    m3u_resolve(out, m3u_path, line);
    return true;
  }
  return false;
}

bool queue_from_playlist(const char *m3u_path)
{
  FIL in, out;
  char p[LIB_PATH_LEN];
  uint32_t n = 0;

  if (f_open(&in, m3u_path, FA_READ) != FR_OK)
  {
    return false;
  }
  if (f_open(&out, QUEUE_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
  {
    f_close(&in);
    return false;
  }
  while (m3u_next(&in, m3u_path, p))
  {
    rec_append(&out, p);
    n++;
  }
  f_close(&out);
  f_close(&in);
  return n > 0U;
}

bool queue_from_library(void)
{
  if (library_count() == 0U && library_rebuild() == 0U)
  {
    return false;
  }
  /* Same record format: the queue is simply a copy of the index. */
  FIL in, out;
  UINT br, bw;
  static uint8_t buf[2048];
  if (f_open(&in, LIBRARY_FILE, FA_READ) != FR_OK)
  {
    return false;
  }
  if (f_open(&out, QUEUE_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
  {
    f_close(&in);
    return false;
  }
  while (f_read(&in, buf, sizeof(buf), &br) == FR_OK && br > 0U)
  {
    f_write(&out, buf, br, &bw);
  }
  f_close(&out);
  f_close(&in);
  return queue_count() > 0U;
}

bool queue_from_single(const char *path)
{
  FIL f;
  if (f_open(&f, QUEUE_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
  {
    return false;
  }
  bool ok = rec_append(&f, path);
  f_close(&f);
  return ok;
}

uint32_t queue_count(void)
{
  return rec_count(QUEUE_FILE);
}

bool queue_get(uint32_t i, char *path)
{
  return rec_get(QUEUE_FILE, i, path);
}

/* ---- playlists ---------------------------------------------------------------- */

/* Fill a list for the Playlists screen: every .m3u in /Playlists, with
 * Favourites (created empty if missing) at the top. */
bool playlists_list(lib_list_t *l)
{
  DIR d;
  l->count = 0;
  l->truncated = false;
  strcpy(l->path, PLAYLIST_DIR);
  if (!storage_exists(FAVOURITES_FILE))
  {
    FIL f;
    if (f_open(&f, FAVOURITES_FILE, FA_WRITE | FA_CREATE_NEW) == FR_OK)
    {
      f_puts("#EXTM3U\n", &f);
      f_close(&f);
    }
  }
  if (f_opendir(&d, PLAYLIST_DIR) != FR_OK)
  {
    return false;
  }
  while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
  {
    const char *dot = strrchr(fi.fname, '.');
    if (!(fi.fattrib & AM_DIR) && dot && strcasecmp(dot, ".m3u") == 0)
    {
      add(l, fi.fname, false);
    }
  }
  f_closedir(&d);
  qsort(l->e, l->count, sizeof(l->e[0]), cmp_entry);
  /* Favourites always first, as in the Figma list */
  for (uint16_t i = 1; i < l->count; i++)
  {
    if (strcasecmp(l->e[i].name, "Favourites.m3u") == 0)
    {
      lib_entry_t t = l->e[i];
      memmove(&l->e[1], &l->e[0], i * sizeof(l->e[0]));
      l->e[0] = t;
      break;
    }
  }
  return true;
}

/* List a playlist's songs by file name (for display). */
bool playlist_songs(lib_list_t *l, const char *m3u_path)
{
  FIL f;
  char p[LIB_PATH_LEN];
  l->count = 0;
  l->truncated = false;
  strncpy(l->path, m3u_path, LIB_PATH_LEN - 1);
  l->path[LIB_PATH_LEN - 1] = '\0';
  if (f_open(&f, m3u_path, FA_READ) != FR_OK)
  {
    return false;
  }
  while (m3u_next(&f, m3u_path, p))
  {
    const char *name = strrchr(p, '/');
    add(l, name ? name + 1 : p, false);
  }
  f_close(&f);
  return true;
}

/* New empty playlist named "My playlist N" with the first free N. */
bool playlist_create(char *path_out)
{
  FIL f;
  for (int n = 1; n < 100; n++)
  {
    snprintf(path_out, LIB_PATH_LEN, PLAYLIST_DIR "/My playlist %d.m3u", n);
    if (f_open(&f, path_out, FA_WRITE | FA_CREATE_NEW) == FR_OK)
    {
      f_puts("#EXTM3U\n", &f);
      f_close(&f);
      return true;
    }
  }
  return false;
}

bool playlist_contains(const char *m3u_path, const char *song_path)
{
  FIL f;
  char p[LIB_PATH_LEN];
  bool found = false;
  if (f_open(&f, m3u_path, FA_READ) != FR_OK)
  {
    return false;
  }
  while (!found && m3u_next(&f, m3u_path, p))
  {
    found = strcasecmp(p, song_path) == 0;
  }
  f_close(&f);
  return found;
}

/* Append a song to a playlist (Centre hold). Already there = nothing to do. */
bool playlist_add(const char *m3u_path, const char *song_path)
{
  FIL f;
  if (playlist_contains(m3u_path, song_path))
  {
    return true;
  }
  if (f_open(&f, m3u_path, FA_WRITE | FA_OPEN_APPEND) != FR_OK)
  {
    return false;
  }
  f_printf(&f, "%s\n", song_path);
  f_close(&f);
  return true;
}

/* ---- shuffle-all index --------------------------------------------------------- */

#define SCAN_DEPTH 6                /* how many folder levels deep to look */

/* Walk `path` and its sub-folders, writing every song found to `out`.
 * Recursive; `path` is one shared buffer that grows and shrinks as it goes
 * in and out of folders, to save stack. Skips hidden files, /System and
 * /Mixtapes (recordings aren't music). Returns the number of songs found. */
static uint32_t scan(FIL *out, char *path, int depth)
{
  DIR d;
  uint32_t n = 0;
  size_t len = strlen(path);

  if (depth > SCAN_DEPTH || f_opendir(&d, path) != FR_OK)
  {
    return 0;
  }
  while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
  {
    if (fi.fname[0] == '.' || (fi.fattrib & (AM_HID | AM_SYS)) ||
        strcmp(fi.fname, "System") == 0 || strcmp(fi.fname, "Mixtapes") == 0)
    {
      continue;
    }
    if (len + strlen(fi.fname) + 2U >= LIB_PATH_LEN)
    {
      continue;
    }
    bool dir = (fi.fattrib & AM_DIR) != 0;
    if (dir)
    {
      char name[LIB_NAME_LEN];
      strncpy(name, fi.fname, sizeof(name) - 1);
      name[sizeof(name) - 1] = '\0';
      snprintf(path + len, LIB_PATH_LEN - len, "/%s", name);
      n += scan(out, path, depth + 1);
      path[len] = '\0';
    }
    else if (lib_is_audio(fi.fname))
    {
      snprintf(path + len, LIB_PATH_LEN - len, "/%s", fi.fname);
      if (rec_append(out, path))
      {
        n++;
      }
      path[len] = '\0';
    }
  }
  f_closedir(&d);
  return n;
}

/* Rescan all music into library.idx. Runs on first "shuffle all" and after
 * USB mode, because files may have changed. */
uint32_t library_rebuild(void)
{
  FIL out;
  static char path[LIB_PATH_LEN];
  uint32_t n = 0;

  if (f_open(&out, LIBRARY_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
  {
    return 0;
  }
  strcpy(path, EMMC_DRIVE "/Music");
  n += scan(&out, path, 0);
  if (storage_sd_mounted())
  {
    strcpy(path, SD_DRIVE);
    n += scan(&out, path, 0);
  }
  f_close(&out);
  return n;
}

uint32_t library_count(void)
{
  return rec_count(LIBRARY_FILE);
}
