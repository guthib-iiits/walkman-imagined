/**
  ******************************************************************************
  * @file    npm1300.c
  * @brief   Nordic nPM1300 PMIC: battery voltage, charge state, USB presence.
  *          Registers are addressed as {base, offset}, sent as a 16-bit address,
 *          over the bit-banged bus on PE12/PE13.
  ******************************************************************************
  */
#include "npm1300.h"
#include "soft_i2c.h"
#include "walkman_config.h"

#define REG(base, off)          ((uint16_t)(((base) << 8) | (off)))

#define VBUSIN_STATUS           REG(0x02, 0x07)   /* bit0: VBUS present */
#define CHGR_ERR_CLR            REG(0x03, 0x00)
#define CHGR_EN_SET             REG(0x03, 0x04)
#define CHGR_CHG_STAT           REG(0x03, 0x34)
#define ADC_TASK_VBAT           REG(0x05, 0x00)
#define ADC_RESULTS             REG(0x05, 0x10)

#define STAT_BATTERY_DETECTED   0x01U
#define STAT_COMPLETED          0x02U
#define STAT_CHARGING_MASK      0x1CU   /* trickle | constant current | constant voltage */

static bool pmic_ok;               /* PMIC answered at boot */

/* Read n bytes starting at register reg. */
static bool rd(uint16_t reg, uint8_t *buf, uint16_t n)
{
  return soft_i2c_read16(NPM1300_I2C_ADDR, reg, buf, n);
}

static bool wr(uint16_t reg, uint8_t v)
{
  return soft_i2c_write16(NPM1300_I2C_ADDR, reg, &v, 1);
}

/* Check the PMIC is there (any successful read will do), enable charging and
 * start the first battery-voltage measurement. */
bool npm1300_init(void)
{
  uint8_t v;
  soft_i2c_init();
  pmic_ok = rd(CHGR_CHG_STAT, &v, 1);
  if (pmic_ok)
  {
    /* Charge current and termination voltage stay at the PMIC defaults until
     * the battery is chosen; this only makes sure charging is allowed. */
    wr(CHGR_ERR_CLR, 1);           /* clear any old charger error */
    wr(CHGR_EN_SET, 1);            /* allow charging */
    wr(ADC_TASK_VBAT, 1);          /* measure VBAT; result is read next update */
  }
  return pmic_ok;
}

/* Typical single-cell Li-ion discharge curve at light load.
 * curve[i] is the voltage at i * 10 %; values in between are interpolated. */
static uint8_t vbat_to_percent(uint16_t mv)
{
  static const uint16_t curve[11] = {
    3300, 3550, 3640, 3700, 3740, 3780, 3830, 3900, 3980, 4080, 4180
  };
  if (mv <= curve[0])
  {
    return 0;
  }
  for (int i = 1; i < 11; i++)
  {
    if (mv < curve[i])
    {
      return (uint8_t)((i - 1) * 10 + (mv - curve[i - 1]) * 10 / (curve[i] - curve[i - 1]));
    }
  }
  return 100;
}

/* Read charger state, USB presence and the last battery measurement, then
 * start the next measurement. Returns false if the PMIC didn't answer. */
bool npm1300_update(npm1300_status_t *s)
{
  uint8_t stat = 0, vbus = 0, res[6];

  s->present = pmic_ok;
  if (!pmic_ok)
  {
    return false;
  }
  if (!rd(CHGR_CHG_STAT, &stat, 1) || !rd(VBUSIN_STATUS, &vbus, 1) ||
      !rd(ADC_RESULTS, res, sizeof(res)))
  {
    return false;
  }
  /* res[1] = VBAT MSB, res[5] bits 1:0 = VBAT LSBs; 10-bit code, 5.0 V full scale */
  uint16_t code = (uint16_t)((res[1] << 2) | (res[5] & 0x03U));
  s->vbat_mv  = (uint16_t)((uint32_t)code * 5000U / 1023U);
  s->percent  = vbat_to_percent(s->vbat_mv);
  s->charging = (stat & STAT_CHARGING_MASK) != 0;
  s->charged  = (stat & STAT_COMPLETED) != 0;
  s->vbus     = (vbus & 0x01U) != 0;

  wr(ADC_TASK_VBAT, 1);
  return true;
}
