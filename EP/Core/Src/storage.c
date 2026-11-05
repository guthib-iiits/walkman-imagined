/**
  ******************************************************************************
  * @file    storage.c
  * @brief   eMMC + SD card bring-up, FatFs mounts and USB mass-storage hand-off.
  *
  *  The SD card is optional, so its init must not end in Error_Handler(). The
  *  CubeMX init calls for SDMMC1/SDMMC2 are disabled ("Do Not Generate Function
  *  Call") and both cards are brought up here instead.
  *
  *  FatFs (the FAT file system library) talks to storage through "disk
  *  drivers". The SD card uses the driver CubeMX generates (sd_diskio.c);
  *  the eMMC uses the small driver in this file. Drive "0:" is the SD card,
  *  "1:" the eMMC.
  *
  *  USB mode: the PC reads and writes eMMC blocks directly. While that is
  *  active (usb_attached), FatFs is unmounted and every FatFs access to the
  *  eMMC is refused, so the two can never write at the same time.
  ******************************************************************************
  */
#include "storage.h"
#include "walkman_config.h"
#include "fatfs.h"
#include "ff_gen_drv.h"
#include "bsp_driver_sd.h"
#include <string.h>

extern SD_HandleTypeDef hsd1;
extern MMC_HandleTypeDef hmmc2;

#define MMC_TIMEOUT 1000U

static FATFS emmc_fs;               /* FatFs bookkeeping for the eMMC volume */
static char  emmc_path[4];          /* "1:/" once the driver is linked */
static storage_state_t emmc_state = STORAGE_NO_DEVICE;
static storage_state_t sd_state = STORAGE_NO_DEVICE;
static bool emmc_hw_ok;             /* eMMC chip answered */
static bool usb_attached;           /* PC owns the eMMC */
static uint8_t mkfs_work[4096];     /* scratch RAM f_mkfs needs to format */

/* ---- SD card (used by the CubeMX sd_diskio.c through BSP_SD_Init) --------- */

/* Overrides CubeMX's weak BSP_SD_Init: start SDMMC1 for the card. Returns an
 * error instead of halting, because having no card is normal. */
uint8_t BSP_SD_Init(void)
{
  hsd1.Instance = SDMMC1;
  hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
  hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsd1.Init.BusWide = SDMMC_BUS_WIDE_4B;
  hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
  hsd1.Init.ClockDiv = 2;                          /* 100 MHz / 4 = 25 MHz */
  HAL_SD_DeInit(&hsd1);
  return (HAL_SD_Init(&hsd1) == HAL_OK) ? MSD_OK : MSD_ERROR;
}

/* ---- eMMC FatFs driver ---------------------------------------------------- */

/* After a read/write the eMMC is busy for a while; wait until it is back in
 * the "transfer" state and ready for the next command. */
static bool mmc_wait_ready(void)
{
  uint32_t t0 = HAL_GetTick();
  while (HAL_MMC_GetCardState(&hmmc2) != HAL_MMC_CARD_TRANSFER)
  {
    if (HAL_GetTick() - t0 > MMC_TIMEOUT)
    {
      return false;
    }
  }
  return true;
}

static bool mmc_read(uint8_t *buf, uint32_t blk, uint32_t n)
{
  return HAL_MMC_ReadBlocks(&hmmc2, buf, blk, n, MMC_TIMEOUT) == HAL_OK && mmc_wait_ready();
}

static bool mmc_write(const uint8_t *buf, uint32_t blk, uint32_t n)
{
  return HAL_MMC_WriteBlocks(&hmmc2, (uint8_t *)buf, blk, n, MMC_TIMEOUT) == HAL_OK && mmc_wait_ready();
}

/* The five functions FatFs calls for drive "1:". Sectors are 512 bytes. */
static DSTATUS emmc_disk_status(BYTE lun)
{
  (void)lun;
  return (emmc_hw_ok && !usb_attached) ? 0 : STA_NOINIT;
}

static DSTATUS emmc_disk_initialize(BYTE lun)
{
  return emmc_disk_status(lun);
}

static DRESULT emmc_disk_read(BYTE lun, BYTE *buf, DWORD sector, UINT count)
{
  (void)lun;
  if (usb_attached)
  {
    return RES_NOTRDY;
  }
  return mmc_read(buf, sector, count) ? RES_OK : RES_ERROR;
}

static DRESULT emmc_disk_write(BYTE lun, const BYTE *buf, DWORD sector, UINT count)
{
  (void)lun;
  if (usb_attached)
  {
    return RES_NOTRDY;
  }
  return mmc_write(buf, sector, count) ? RES_OK : RES_ERROR;
}

static DRESULT emmc_disk_ioctl(BYTE lun, BYTE cmd, void *buf)
{
  (void)lun;
  HAL_MMC_CardInfoTypeDef info;
  switch (cmd)
  {
    case CTRL_SYNC:
      return mmc_wait_ready() ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
      HAL_MMC_GetCardInfo(&hmmc2, &info);
      *(DWORD *)buf = info.LogBlockNbr;
      return RES_OK;
    case GET_SECTOR_SIZE:
      *(WORD *)buf = 512;
      return RES_OK;
    case GET_BLOCK_SIZE:
      *(DWORD *)buf = 1024;               /* erase block, in sectors (512 KiB) */
      return RES_OK;
    default:
      return RES_PARERR;
  }
}

static const Diskio_drvTypeDef emmc_driver = {
  emmc_disk_initialize,
  emmc_disk_status,
  emmc_disk_read,
#if _USE_WRITE == 1
  emmc_disk_write,
#endif
#if _USE_IOCTL == 1
  emmc_disk_ioctl,
#endif
};

/* Start SDMMC2 and identify the eMMC chip. */
static bool emmc_hw_init(void)
{
  hmmc2.Instance = SDMMC2;
  hmmc2.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
  hmmc2.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hmmc2.Init.BusWide = SDMMC_BUS_WIDE_4B;
  hmmc2.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
  hmmc2.Init.ClockDiv = 2;                         /* 25 MHz, legacy eMMC timing */
  return HAL_MMC_Init(&hmmc2) == HAL_OK;
}

/* ---- mounting ------------------------------------------------------------- */

/* Mount a volume and turn the FatFs result into our simpler states
 * (OK / not formatted / no card / error). */
static storage_state_t mount(FATFS *fs, const char *path)
{
  FRESULT r = f_mount(fs, path, 1);
  if (r == FR_OK)
  {
    return STORAGE_OK;
  }
  if (r == FR_NO_FILESYSTEM)
  {
    return STORAGE_NO_FILESYSTEM;
  }
  return (r == FR_NOT_READY) ? STORAGE_NO_DEVICE : STORAGE_ERROR;
}

/* Boot: bring up the eMMC, mount it, create the standard folders, then try
 * the SD card. */
void storage_init(void)
{
  emmc_hw_ok = emmc_hw_init();
  FATFS_LinkDriver(&emmc_driver, emmc_path);       /* SD was linked first: this is "1:" */

  emmc_state = emmc_hw_ok ? mount(&emmc_fs, EMMC_DRIVE) : STORAGE_NO_DEVICE;
  if (emmc_state == STORAGE_OK)
  {
    storage_mkdirs(SYSTEM_DIR);
    storage_mkdirs(PLAYLIST_DIR);
    storage_mkdirs(EMMC_DRIVE "/Music");
  }
  storage_sd_rescan();
}

/* (Re)try the SD card. There is no working card-detect line, so this is
 * called whenever the SD card is needed (e.g. starting a mixtape). */
void storage_sd_rescan(void)
{
  sd_state = mount(&SDFatFS, SDPath);
  if (sd_state == STORAGE_OK)
  {
    storage_mkdirs(MIXTAPE_DIR);
  }
}

storage_state_t storage_emmc_state(void) { return emmc_state; }
storage_state_t storage_sd_state(void)   { return sd_state; }
bool storage_sd_mounted(void)            { return sd_state == STORAGE_OK; }
bool storage_emmc_mounted(void)          { return emmc_state == STORAGE_OK && !usb_attached; }

uint32_t storage_emmc_mb(void)
{
  if (!emmc_hw_ok)
  {
    return 0;
  }
  HAL_MMC_CardInfoTypeDef info;
  HAL_MMC_GetCardInfo(&hmmc2, &info);
  return (uint32_t)(((uint64_t)info.LogBlockNbr * info.LogBlockSize) >> 20);
}

/* Erase and create a new file system (FAT32 or exFAT, FatFs picks by size),
 * then mount it. Settings -> Format. */
static bool format(const char *path, FATFS *fs, storage_state_t *state)
{
  f_mount(NULL, path, 0);
  FRESULT r = f_mkfs(path, FM_ANY, 0, mkfs_work, sizeof(mkfs_work));
  *state = (r == FR_OK) ? mount(fs, path) : STORAGE_ERROR;
  return *state == STORAGE_OK;
}

bool storage_format_emmc(void)
{
  if (!emmc_hw_ok || usb_attached)
  {
    return false;
  }
  bool ok = format(EMMC_DRIVE, &emmc_fs, &emmc_state);
  if (ok)
  {
    storage_mkdirs(SYSTEM_DIR);
    storage_mkdirs(PLAYLIST_DIR);
    storage_mkdirs(EMMC_DRIVE "/Music");
  }
  return ok;
}

bool storage_format_sd(void)
{
  if (BSP_SD_Init() != MSD_OK)
  {
    sd_state = STORAGE_NO_DEVICE;
    return false;
  }
  bool ok = format(SD_DRIVE, &SDFatFS, &sd_state);
  if (ok)
  {
    storage_mkdirs(MIXTAPE_DIR);
  }
  return ok;
}

/* ---- USB mass storage ------------------------------------------------------ */

/* The storage_usb_* functions are called by usbd_storage_if.c (the USB
 * mass-storage class) to answer the PC's block reads and writes. */

void storage_usb_attach(void)
{
  if (usb_attached || !emmc_hw_ok)
  {
    return;
  }
  f_mount(NULL, EMMC_DRIVE, 0);
  usb_attached = true;
}

void storage_usb_detach(void)
{
  if (!usb_attached)
  {
    return;
  }
  usb_attached = false;
  emmc_state = mount(&emmc_fs, EMMC_DRIVE);
}

bool storage_usb_attached(void)
{
  return usb_attached;
}

bool storage_usb_capacity(uint32_t *blocks, uint16_t *block_size)
{
  if (!emmc_hw_ok)
  {
    return false;
  }
  HAL_MMC_CardInfoTypeDef info;
  HAL_MMC_GetCardInfo(&hmmc2, &info);
  *blocks = info.LogBlockNbr;
  *block_size = 512;
  return true;
}

bool storage_usb_read(uint8_t *buf, uint32_t blk, uint16_t n)
{
  return usb_attached && mmc_read(buf, blk, n);
}

bool storage_usb_write(const uint8_t *buf, uint32_t blk, uint16_t n)
{
  return usb_attached && mmc_write(buf, blk, n);
}

/* ---- helpers --------------------------------------------------------------- */

bool storage_exists(const char *path)
{
  FILINFO fi;
  return f_stat(path, &fi) == FR_OK;
}

/* Create a folder and any missing parents, like "mkdir -p". */
bool storage_mkdirs(const char *path)
{
  char tmp[128];
  size_t len = strlen(path);
  if (len >= sizeof(tmp))
  {
    return false;
  }
  memcpy(tmp, path, len + 1);
  for (size_t i = 3; i <= len; i++)            /* skip "N:/" */
  {
    if (tmp[i] == '/' || tmp[i] == '\0')
    {
      char c = tmp[i];
      tmp[i] = '\0';
      FRESULT r = f_mkdir(tmp);
      if (r != FR_OK && r != FR_EXIST)
      {
        return false;
      }
      tmp[i] = c;
    }
  }
  return true;
}
