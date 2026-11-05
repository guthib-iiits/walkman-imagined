/**
  ******************************************************************************
  * @file    rtc_clock.h
  * @brief   Wall clock on the RTC, clocked from the 32.768 kHz LSE crystal.
  *          The RTC is not enabled in CubeMX, so this drives the registers
  *          directly and needs no HAL_RTC module.
  ******************************************************************************
  */
#ifndef RTC_CLOCK_H
#define RTC_CLOCK_H

#include <stdint.h>
#include <stdbool.h>

bool rtc_clock_init(void);           /* false if the LSE crystal did not start */
bool rtc_clock_valid(void);          /* time was set at least once */
void rtc_clock_get(uint8_t *hh, uint8_t *mm, uint8_t *ss);
void rtc_clock_set(uint8_t hh, uint8_t mm);
uint32_t rtc_clock_fattime(void);    /* FatFs timestamp */

#endif /* RTC_CLOCK_H */
