/**
  ******************************************************************************
  * @file    rtc_clock.c
  * @brief   Wall clock on the RTC, clocked from the 32.768 kHz LSE crystal.
  *
  *  The RTC sits in the backup domain, so it keeps running through standby
  *  ("power off") and resets. It is only set up once, on the very first
  *  boot; after that rtc_clock_init() sees it already running and leaves it.
  *  Registers are written directly instead of through HAL_RTC, which would
  *  need CubeMX's RTC peripheral enabled.
  ******************************************************************************
  */
#include "rtc_clock.h"
#include "main.h"

#define RTC_MAGIC 0x57414C4BU   /* "WALK": time has been set */

static bool rtc_ok;             /* LSE crystal started and RTC running */

/* The RTC stores time as BCD: 0x59 means 59. */
static uint8_t bcd(uint8_t v)   { return (uint8_t)(((v / 10U) << 4) | (v % 10U)); }
static uint8_t unbcd(uint32_t v) { return (uint8_t)(((v >> 4) & 0xFU) * 10U + (v & 0xFU)); }

/* RTC registers are write-protected; this key sequence unlocks them. */
static void rtc_unlock(void) { RTC->WPR = 0xCAU; RTC->WPR = 0x53U; }
static void rtc_lock(void)   { RTC->WPR = 0xFFU; }

/* Stop the clock so time / date can be changed. */
static bool rtc_enter_init(void)
{
  RTC->ISR |= RTC_ISR_INIT;
  uint32_t t0 = HAL_GetTick();
  while (!(RTC->ISR & RTC_ISR_INITF))
  {
    if (HAL_GetTick() - t0 > 100U)
    {
      return false;
    }
  }
  return true;
}

bool rtc_clock_init(void)
{
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_RTC_CLK_ENABLE();

  if (!(RCC->BDCR & RCC_BDCR_RTCEN))
  {
    __HAL_RCC_LSE_CONFIG(RCC_LSE_ON);
    uint32_t t0 = HAL_GetTick();
    while (!__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY))
    {
      if (HAL_GetTick() - t0 > 3000U)
      {
        rtc_ok = false;
        return false;
      }
    }
    __HAL_RCC_RTC_CONFIG(RCC_RTCCLKSOURCE_LSE);
    __HAL_RCC_RTC_ENABLE();

    rtc_unlock();
    if (rtc_enter_init())
    {
      RTC->PRER = (127U << RTC_PRER_PREDIV_A_Pos) | 255U;   /* 1 Hz from 32768 Hz */
      RTC->CR &= ~RTC_CR_FMT;                                /* 24 h */
      RTC->TR = 0;
      RTC->DR = (0x26U << RTC_DR_YU_Pos) | (1U << RTC_DR_MU_Pos) | 1U; /* 2026-01-01 */
      RTC->ISR &= ~RTC_ISR_INIT;
    }
    rtc_lock();
  }
  rtc_ok = true;
  return true;
}

/* false until the user has set the time once (the UI hides the clock until then). */
bool rtc_clock_valid(void)
{
  return rtc_ok && RTC->BKP0R == RTC_MAGIC;
}

void rtc_clock_get(uint8_t *hh, uint8_t *mm, uint8_t *ss)
{
  uint32_t tr = RTC->TR;
  (void)RTC->DR;                       /* unlocks the shadow registers */
  *hh = unbcd((tr >> RTC_TR_HU_Pos) & 0x3FU);
  *mm = unbcd((tr >> RTC_TR_MNU_Pos) & 0x7FU);
  *ss = unbcd(tr & 0x7FU);
}

/* Settings -> Set Time. Seconds restart at 0. */
void rtc_clock_set(uint8_t hh, uint8_t mm)
{
  if (!rtc_ok)
  {
    return;
  }
  rtc_unlock();
  if (rtc_enter_init())
  {
    RTC->TR = ((uint32_t)bcd(hh) << RTC_TR_HU_Pos) | ((uint32_t)bcd(mm) << RTC_TR_MNU_Pos);
    RTC->ISR &= ~RTC_ISR_INIT;
  }
  rtc_lock();
  RTC->BKP0R = RTC_MAGIC;
}

/* Time stamp for files FatFs creates (mixtapes, settings), packed the way
 * FAT stores it: year-1980 | month | day | hour | minute | seconds/2. */
uint32_t rtc_clock_fattime(void)
{
  if (!rtc_ok)
  {
    return ((uint32_t)(2026 - 1980) << 25) | (1U << 21) | (1U << 16);
  }
  uint32_t tr = RTC->TR;
  uint32_t dr = RTC->DR;
  uint32_t year = 2000U + unbcd((dr >> RTC_DR_YU_Pos) & 0xFFU);
  uint32_t mon  = unbcd((dr >> RTC_DR_MU_Pos) & 0x1FU);
  uint32_t day  = unbcd(dr & 0x3FU);
  uint32_t hh   = unbcd((tr >> RTC_TR_HU_Pos) & 0x3FU);
  uint32_t mm   = unbcd((tr >> RTC_TR_MNU_Pos) & 0x7FU);
  uint32_t ss   = unbcd(tr & 0x7FU);
  return ((year - 1980U) << 25) | (mon << 21) | (day << 16) | (hh << 11) | (mm << 5) | (ss / 2U);
}
