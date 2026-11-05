/**
  ******************************************************************************
  * @file    app.c
  * @brief   EP Walkman application: boot sequence, main loop and power.
  *
  *  Boot (Figma "On Bootup"): e-ink init -> battery status from the nPM1300 ->
  *  storage -> Hysteresis (resume last session, can be turned off in Settings)
  *  -> display menu. "Turn on the DAC" happens at boot so play starts instantly.
  *
  *  How the firmware runs: main.c calls app_init() once, then app_loop()
  *  forever. There is no RTOS. Every module has a *_task() function that does a
  *  little work and returns quickly, and the loop calls them in turn
  *  (buttons -> player -> mixtape -> UI -> display -> battery). Audio keeps
  *  playing in the background because the DMA streams samples to the DACs by
  *  itself; the loop only has to top up the buffer in time.
  ******************************************************************************
  */
#include "app.h"
#include "audio_out.h"
#include "buttons.h"
#include "cs43131.h"
#include "display.h"
#include "epd.h"
#include "library.h"
#include "mixtape.h"
#include "npm1300.h"
#include "player.h"
#include "rtc_clock.h"
#include "settings.h"
#include "storage.h"
#include "ui.h"
#include "usb_device.h"
#include "usbd_core.h"
#include "walkman_config.h"

extern USBD_HandleTypeDef hUsbDeviceFS;

#define PMIC_POLL_MS 10000U          /* read battery / USB state every 10 s */

static bool dacs_ok;                 /* both CS43131 DACs answered at boot */
static bool usb_on;                  /* eMMC is handed to the PC as a USB drive */
static bool last_vbus;               /* USB cable present at the last poll */
static uint32_t last_pmic_poll;
static uint32_t sleep_deadline;      /* tick when the sleep timer fires, 0 = off */
static uint8_t low_batt_count;       /* consecutive low readings before shutdown */

/* The UI asks this to show "DAC not found" instead of a silent player. */
bool app_dacs_ok(void)
{
  return dacs_ok;
}

/* (Re)start the Settings -> Sleep Timer countdown. Called at boot, when the
 * setting changes, and on every button press. */
void app_sleep_timer_restart(void)
{
  sleep_deadline = g_settings.sleep_min ? HAL_GetTick() + g_settings.sleep_min * 60000U : 0U;
}

/* Settings -> Low Power changed: just redraw. The idle check in app_loop()
 * reads g_settings.low_power directly. */
void app_apply_low_power(void)
{
  ui_invalidate();
}

/* Runs once after CubeMX has set up the clocks and peripherals. Brings every
 * module up in the order from the Figma boot flow and picks the first screen. */
void app_init(void)
{
  npm1300_status_t batt = {0};
  resume_t r;
  ui_start_t start = UI_START_MENU;

  /* D-cache stays off: every DMA buffer would otherwise need cache maintenance. */
  SCB_EnableICache();

  buttons_init();
  rtc_clock_init();
  epd_init();
  display_init();

  /* Battery first: if it is nearly empty, don't spin up storage and audio. */
  if (npm1300_init())
  {
    HAL_Delay(5);                  /* give the PMIC's ADC time for a first reading */
    npm1300_update(&batt);
    last_vbus = batt.vbus;
    if (batt.vbat_mv && batt.vbat_mv < BATTERY_CUTOFF_MV && !batt.vbus)
    {
      start = UI_START_LOW_BATTERY;
    }
  }

  /* Mount eMMC (+ SD card if present), then read settings from it. */
  storage_init();
  settings_load();
  app_sleep_timer_restart();

  /* Bring both DACs out of reset and start the (silent) SAI stream. */
  dacs_ok = audio_out_init();
  audio_out_set_volume(g_settings.volume);
  player_init();

  /* Choose the first screen: format prompt, resume, or the main menu. */
  if (start == UI_START_MENU && storage_emmc_state() == STORAGE_NO_FILESYSTEM)
  {
    start = UI_START_FORMAT_EMMC;
  }
  else if (start == UI_START_MENU && g_settings.hysteresis)
  {
    resume_load(&r);
    if (r.valid && queue_count() > r.queue_index && player_start(r.queue_index, r.position_ms))
    {
      player_pause();              /* back where you left off; Centre to play */
      start = UI_START_NOW_PLAYING;
    }
  }
  ui_init(start);
}

/* Every PMIC_POLL_MS: offer USB mode when a cable is plugged in, and shut down
 * cleanly after three low-battery readings in a row (one bad reading, e.g.
 * during a current spike, is not enough). */
static void poll_pmic(uint32_t now)
{
  npm1300_status_t s;
  if (now - last_pmic_poll < PMIC_POLL_MS)
  {
    return;
  }
  last_pmic_poll = now;
  if (!npm1300_update(&s))
  {
    return;
  }
  if (s.vbus && !last_vbus && !usb_on && storage_emmc_mounted())
  {
    ui_offer_usb();
  }
  last_vbus = s.vbus;

  if (s.vbat_mv && s.vbat_mv < BATTERY_CUTOFF_MV && !s.vbus)
  {
    if (++low_batt_count >= 3U)
    {
      ui_message_blocking("Battery low", "Shutting down");
      app_shutdown();
    }
  }
  else
  {
    low_batt_count = 0;
  }
}

/* One pass of the main loop. main.c calls this forever. */
void app_loop(void)
{
  btn_event_t ev;
  uint32_t now = HAL_GetTick();

  /* Hand every new button event to the UI; any press restarts the sleep timer. */
  while (buttons_poll(&ev))
  {
    ui_input(&ev);
    if (g_settings.sleep_min)
    {
      app_sleep_timer_restart();
    }
  }

  player_task();                   /* decode MP3 into the audio buffer */
  mixtape_task();                  /* record mic / write WAV when a mixtape runs */
  ui_task();                       /* redraw the current screen into RAM if needed */
  display_task();                  /* push changed pixels to the e-paper */
  poll_pmic(now);                  /* battery + USB cable */

  pl_info_t pi;
  player_info(&pi);

  if (sleep_deadline && (int32_t)(now - sleep_deadline) >= 0)
  {
    ui_message_blocking("Sleep timer", "Good night");
    app_shutdown();
  }

  /* Paused or stopped for a while: power the audio path down, then the MCU. */
  if (pi.state != PL_PLAYING && !usb_on && mixtape_state() != MT_RECORDING)
  {
    uint32_t idle = now - buttons_last_activity();
    if (idle > 30000U && !audio_out_suspended())
    {
      audio_out_suspend();
    }
    if (g_settings.low_power && idle > IDLE_STANDBY_MS && !ui_refreshing())
    {
      app_shutdown();
    }
  }

  /* Nothing to decode right now: sleep until the next interrupt (SysTick, DMA). */
  if (audio_out_space() < MP3S_MAX_OUT_FRAMES || pi.state != PL_PLAYING)
  {
    __WFI();
  }
}

/* Save everything (resume point, settings) and quiet the hardware. Shared by
 * shutdown and restart. */
static void power_down_peripherals(void)
{
  if (mixtape_state() == MT_RECORDING)
  {
    mixtape_stop();
  }
  player_save_resume();
  player_stop();
  settings_save();
  if (usb_on)
  {
    app_usb_exit();
  }
  cs43131_stop();
  audio_out_suspend();
  HAL_GPIO_WritePin(DAC1_RST_GPIO_Port, DAC1_RST_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(DAC2_RST_GPIO_Port, DAC2_RST_Pin, GPIO_PIN_RESET);
}

/* "Power off": the STM32 enters standby (a few uA). The e-paper keeps showing
 * the last image with no power. Pressing Power (PC13 = WKUP4) resets the MCU
 * and the firmware boots from the start. */
void app_shutdown(void)
{
  power_down_peripherals();
  ui_message_blocking("EP Walkman", "Power to wake");
  epd_sleep();

  /* Wait for the power button to be released, or it wakes us straight away. */
  btn_event_t ev;
  uint32_t t0 = HAL_GetTick();
  while (buttons_any_down() && HAL_GetTick() - t0 < 5000U)
  {
    buttons_poll(&ev);
  }

  PWREx_WakeupPinTypeDef wk = {0};
  wk.WakeUpPin = PWR_WAKEUP_PIN4;
  wk.PinPolarity = BTN_POWER_ACTIVE_LOW ? PWR_PIN_POLARITY_LOW : PWR_PIN_POLARITY_HIGH;
  wk.PinPull = BTN_POWER_ACTIVE_LOW ? PWR_PIN_PULL_UP : PWR_PIN_PULL_DOWN;
  HAL_PWREx_EnableWakeUpPin(&wk);
  HAL_PWREx_ClearWakeupFlag(PWR_WAKEUP_FLAG4);
  HAL_PWR_EnterSTANDBYMode();          /* wakes through reset */
  for (;;)
  {
  }
}

void app_restart(void)
{
  power_down_peripherals();
  epd_sleep();
  NVIC_SystemReset();
}

/* USB drive mode: stop using the eMMC ourselves and let the PC own it.
 * FatFs must not touch the card while the PC writes to it, or the file
 * system gets corrupted. */
void app_usb_enter(void)
{
  if (usb_on)
  {
    return;
  }
  if (mixtape_state() == MT_RECORDING)
  {
    mixtape_stop();
  }
  player_save_resume();
  player_stop();
  settings_save();
  storage_usb_attach();
  USBD_Start(&hUsbDeviceFS);
  usb_on = true;
}

/* Back from USB mode: remount and rescan, since the PC may have added or
 * deleted songs and playlists. */
void app_usb_exit(void)
{
  if (!usb_on)
  {
    return;
  }
  USBD_Stop(&hUsbDeviceFS);
  storage_usb_detach();
  usb_on = false;
  settings_load();
  library_rebuild();
}
