/**
  ******************************************************************************
  * @file    walkman_config.h
  * @brief   Board wiring and feature switches for the EP Walkman.
  *          Everything that depends on how the PCB is wired lives here, so a
  *          wiring change is a one-line edit.
  ******************************************************************************
  */
#ifndef WALKMAN_CONFIG_H
#define WALKMAN_CONFIG_H

#include "main.h"

/* ---- I2C buses (from the Altium schematic, Audio + STM32H7 sheets) ---------
 * Each CS43131 has its own bus: DAC_L on I2C2 (PB10/PB11), DAC_R on I2C1
 * (PB6/PB7). The nPM1300 hangs off PE12 (SDA) / PE13 (SCL), which have no
 * hardware I2C, so it is bit-banged (soft_i2c.c). */
extern I2C_HandleTypeDef hi2c1;
extern I2C_HandleTypeDef hi2c2;
#define DAC_L_I2C             (&hi2c2)
#define DAC_R_I2C             (&hi2c1)
#define PMIC_SDA_PORT         GPIOE
#define PMIC_SDA_PIN          GPIO_PIN_12
#define PMIC_SCL_PORT         GPIOE
#define PMIC_SCL_PIN          GPIO_PIN_13

/* 7-bit addresses. DAC_L has ADR tied to GND (0x30). DAC_R's ADR pin is
 * unconnected on the schematic, so the driver also tries 0x31. */
#define NPM1300_I2C_ADDR      0x6BU
#define CS43131_ADDR          0x30U
#define CS43131_ADDR_ALT      0x31U

/* ---- Audio ------------------------------------------------------------------
 * SAI1 block A (master, MCLK out) -> DAC_L (SD_A), block B (sync slave) ->
 * DAC_R (SD_B). The board has one TRS jack: DAC_L HPOUTA drives the tip,
 * DAC_R HPOUTA the ring, so each DAC plays one channel (dual mono).
 * Set to 0 if a later board revision gives each DAC its own stereo jack. */
#define AUDIO_DUAL_MONO       1
#define AUDIO_DEFAULT_VOLUME  70U          /* 0..100 */
#define AUDIO_VOLUME_STEP     4U

/* I2S2 microphone (INMP441). Its L/R pin is unconnected on the schematic;
 * tie it to GND for the left slot (set 0 here if it is tied high). */
#define MIC_USE_LEFT_SLOT     1

/* ---- Buttons ---------------------------------------------------------------
 * All five D-pad switches and the power switch short to GND (internal pull-ups
 * on the D-pad, R9 10 k pull-up on PC13 / WKUP4). */
#define BTN_ACTIVE_LOW        1
#define BTN_POWER_ACTIVE_LOW  1

/* ---- Storage ---------------------------------------------------------------
 * eMMC (SDMMC2) holds settings + main library and is the USB drive.
 * SD card (SDMMC1) is optional: extra music and mixtape recordings. */
#define EMMC_DRIVE            "1:"         /* FatFs drive numbers follow link order: */
#define SD_DRIVE              "0:"         /* MX_FATFS_Init links the SD card first  */
#define SYSTEM_DIR            EMMC_DRIVE "/System"
#define PLAYLIST_DIR          EMMC_DRIVE "/Playlists"
#define MIXTAPE_DIR           SD_DRIVE "/Mixtapes"
#define SETTINGS_FILE         SYSTEM_DIR "/settings.cfg"
#define RESUME_FILE           SYSTEM_DIR "/resume.cfg"
#define QUEUE_FILE            SYSTEM_DIR "/queue.idx"
#define LIBRARY_FILE          SYSTEM_DIR "/library.idx"
#define FAVOURITES_FILE       PLAYLIST_DIR "/Favourites.m3u"

/* ---- Display ---------------------------------------------------------------
 * Waveshare 2.13" e-Paper (B) V3, 104 x 212, black/white/red, UC8151D.
 * Black/white changes go out as fast partial-window updates (~0.5 s). Red only
 * shows after a full colour refresh (~15 s), which display.c runs once the
 * buttons have been left alone for DISPLAY_COLOUR_DELAY_MS. */
#define EPD_FAST_FRAMES         0x28U    /* fast waveform length at 100 Hz; raise if faint */
#define DISPLAY_COLOUR_DELAY_MS 6000U
#define DISPLAY_CLEAN_EVERY     40U      /* fast updates before a ghost-clearing colour pass */
#define DISPLAY_SLEEP_MS        5000U    /* panel power-down after the last update */
#define UI_REDRAW_SETTLE_MS     30U
#define UI_PROGRESS_PERIOD_MS   (10U * 1000U)

/* ---- Power -----------------------------------------------------------------*/
#define IDLE_STANDBY_MS       (5U * 60U * 1000U)   /* paused + no input, Low Power on */
#define BATTERY_CUTOFF_MV     3300U

#endif /* WALKMAN_CONFIG_H */
