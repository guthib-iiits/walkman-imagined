/**
  ******************************************************************************
  * @file    display.h
  * @brief   Decides how each new frame reaches the e-paper.
  *
  *  The UI renders a complete frame into RAM (about a millisecond) and hands it
  *  over. This state machine diffs it against what is on the glass and only
  *  refreshes the rectangle that changed, using the fast black/white waveform.
  *  Red is deferred: new red pixels show black at first, and one full colour
  *  refresh runs after the buttons have been idle for a few seconds.
  *
  *    IDLE --(black/white diff)--> FAST --(BUSY clears)--> IDLE
  *    IDLE --(red differs, idle)--> COLOUR --(BUSY clears)--> IDLE
  *    COLOUR --(new frame)--> abort --> IDLE (next fast update covers everything)
  *    IDLE --(nothing to do for DISPLAY_SLEEP_MS)--> OFF
  ******************************************************************************
  */
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

void display_init(void);
void display_submit(const uint8_t *k, const uint8_t *r);
void display_task(void);
bool display_busy(void);           /* a refresh is running */
bool display_colour_running(void); /* the slow pass a new frame would cancel */
bool display_fast_pending(void);   /* black/white work not yet on the glass */
void display_request_colour(void); /* colour pass at the next idle moment */

#endif /* DISPLAY_H */
