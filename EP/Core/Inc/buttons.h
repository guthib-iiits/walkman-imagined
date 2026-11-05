/**
  ******************************************************************************
  * @file    buttons.h
  * @brief   Debounced D-pad, centre and power buttons with click / hold / repeat.
  ******************************************************************************
  */
#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
  BTN_UP = 0,
  BTN_DOWN,
  BTN_LEFT,
  BTN_RIGHT,
  BTN_CENTER,
  BTN_POWER,
  BTN_COUNT
} btn_id_t;

typedef enum {
  BTN_EV_NONE = 0,
  BTN_EV_CLICK,       /* released before the hold time */
  BTN_EV_HOLD,        /* held for BTN_HOLD_MS (fires once) */
  BTN_EV_REPEAT,      /* fires every BTN_REPEAT_MS while still held */
  BTN_EV_RELEASE      /* released after a hold */
} btn_ev_type_t;

typedef struct {
  btn_id_t id;
  btn_ev_type_t type;
} btn_event_t;

void buttons_init(void);
/* Call often (every main loop pass). Returns true and fills ev when an event is ready. */
bool buttons_poll(btn_event_t *ev);
bool buttons_any_down(void);
uint32_t buttons_last_activity(void);

#endif /* BUTTONS_H */
