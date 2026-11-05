/**
  ******************************************************************************
  * @file    albumart.h
  * @brief   Album art: embedded ID3 JPEG or cover.jpg / folder.jpg, decoded with
  *          TJpgDec and dithered to the panel's black / white / red.
  ******************************************************************************
  */
#ifndef ALBUMART_H
#define ALBUMART_H

#include <stdint.h>
#include <stdbool.h>

#define ART_SIZE 104U

/* Loads art for a track. offset/size describe an embedded JPEG (0 = none),
 * in which case the folder is searched for cover.jpg / folder.jpg. */
bool albumart_load(const char *track_path, uint32_t offset, uint32_t size);
bool albumart_valid(void);
void albumart_draw(int x, int y);

#endif /* ALBUMART_H */
