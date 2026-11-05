/**
  ******************************************************************************
  * @file    storage.h
  * @brief   eMMC + SD card bring-up, FatFs mounts and USB mass-storage hand-off.
  ******************************************************************************
  */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "ff.h"

typedef enum {
  STORAGE_OK = 0,
  STORAGE_NO_DEVICE,
  STORAGE_NO_FILESYSTEM,
  STORAGE_ERROR
} storage_state_t;

void            storage_init(void);
storage_state_t storage_emmc_state(void);
storage_state_t storage_sd_state(void);
bool            storage_sd_mounted(void);
bool            storage_emmc_mounted(void);
uint32_t        storage_emmc_mb(void);

/* Re-checks the SD slot (card inserted or removed). */
void            storage_sd_rescan(void);

/* Erases and formats. These destroy everything on the device. */
bool            storage_format_emmc(void);
bool            storage_format_sd(void);

/* USB mass storage: the PC gets raw access to the eMMC, so FatFs lets go of it. */
void            storage_usb_attach(void);
void            storage_usb_detach(void);
bool            storage_usb_attached(void);

/* Block access used by usbd_storage_if.c */
bool            storage_usb_capacity(uint32_t *blocks, uint16_t *block_size);
bool            storage_usb_read(uint8_t *buf, uint32_t blk, uint16_t n);
bool            storage_usb_write(const uint8_t *buf, uint32_t blk, uint16_t n);

/* Small helpers */
bool            storage_mkdirs(const char *path);
bool            storage_exists(const char *path);

#endif /* STORAGE_H */
