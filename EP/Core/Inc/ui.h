/**
  ******************************************************************************
  * @file    ui.h
  * @brief   Screens from the Figma "EP/Walkman" board and their navigation.
  ******************************************************************************
  */
#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <stdbool.h>
#include "buttons.h"

typedef enum {
  UI_START_MENU = 0,
  UI_START_NOW_PLAYING,
  UI_START_FORMAT_EMMC,
  UI_START_LOW_BATTERY
} ui_start_t;

void ui_init(ui_start_t start);
void ui_input(const btn_event_t *ev);
void ui_task(void);
void ui_invalidate(void);
bool ui_refreshing(void);
/* Shows a full-screen message and waits for the refresh (shutdown, errors). */
void ui_message_blocking(const char *line1, const char *line2);
/* App asks the UI to offer USB drive mode (VBUS appeared). */
void ui_offer_usb(void);

#endif /* UI_H */
