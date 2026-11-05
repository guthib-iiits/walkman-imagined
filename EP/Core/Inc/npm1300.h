/**
  ******************************************************************************
  * @file    npm1300.h
  * @brief   Nordic nPM1300 PMIC: battery voltage, charge state, USB presence.
  *          Register map from Nordic's nPM1300 datasheet / Zephyr driver.
  ******************************************************************************
  */
#ifndef NPM1300_H
#define NPM1300_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
  bool present;          /* PMIC answered on I2C */
  uint16_t vbat_mv;
  uint8_t percent;       /* 0..100, from a Li-ion voltage curve */
  bool charging;
  bool charged;
  bool vbus;             /* USB power connected */
} npm1300_status_t;

bool npm1300_init(void);
/* Reads the last ADC result and triggers the next measurement. */
bool npm1300_update(npm1300_status_t *s);

#endif /* NPM1300_H */
