/**
  ******************************************************************************
  * @file    app.h
  * @brief   EP Walkman application: boot sequence, main loop and power.
  ******************************************************************************
  */
#ifndef APP_H
#define APP_H

#include <stdbool.h>

/* Called from main() after the CubeMX init calls (USER CODE 2 / USER CODE 3). */
void app_init(void);
void app_loop(void);

void app_shutdown(void);
void app_restart(void);
void app_usb_enter(void);
void app_usb_exit(void);
void app_apply_low_power(void);
void app_sleep_timer_restart(void);
bool app_dacs_ok(void);

#endif /* APP_H */
