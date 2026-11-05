/**
  ******************************************************************************
  * @file    library.h
  * @brief   Folder browsing, .m3u playlists, the shuffle-all index and the
  *          play queue. Long lists live in fixed-size records on the eMMC so
  *          RAM use stays flat no matter how big the library is.
  ******************************************************************************
  */
#ifndef LIBRARY_H
#define LIBRARY_H

#include <stdint.h>
#include <stdbool.h>

#define LIB_MAX_ENTRIES 256U
#define LIB_NAME_LEN    64U
#define LIB_PATH_LEN    256U

typedef struct {
  char name[LIB_NAME_LEN];
  bool is_dir;
} lib_entry_t;

typedef struct {
  char path[LIB_PATH_LEN];
  uint16_t count;
  bool truncated;
  lib_entry_t e[LIB_MAX_ENTRIES];
} lib_list_t;

bool     lib_is_audio(const char *name);
/* Folders first, then songs, both sorted. "" lists the drive roots. */
bool     lib_list_dir(lib_list_t *l, const char *path);
void     lib_join(char *out, const char *dir, const char *name);

/* Play queue (QUEUE_FILE). Returns false if nothing playable was found. */
bool     queue_from_dir(const lib_list_t *l, uint16_t entry, uint32_t *index);
bool     queue_from_playlist(const char *m3u_path);
bool     queue_from_library(void);
bool     queue_from_single(const char *path);
uint32_t queue_count(void);
bool     queue_get(uint32_t i, char *path);

/* Playlists in PLAYLIST_DIR */
bool     playlists_list(lib_list_t *l);
bool     playlist_songs(lib_list_t *l, const char *m3u_path);
bool     playlist_create(char *path_out);
bool     playlist_add(const char *m3u_path, const char *song_path);
bool     playlist_contains(const char *m3u_path, const char *song_path);

/* Shuffle-all index (LIBRARY_FILE) built from eMMC /Music and the SD card. */
uint32_t library_rebuild(void);
uint32_t library_count(void);

#endif /* LIBRARY_H */
