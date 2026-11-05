/**
  ******************************************************************************
  * @file    soft_i2c.h
  * @brief   Bit-banged I2C master. The PCB wires the nPM1300 to PE12 (SDA) and
  *          PE13 (SCL), which have no I2C alternate function on the H743.
  ******************************************************************************
  */
#ifndef SOFT_I2C_H
#define SOFT_I2C_H

#include <stdint.h>
#include <stdbool.h>

void soft_i2c_init(void);
/* Register access with a 16-bit register address (MSB first), as the nPM1300 uses. */
bool soft_i2c_read16(uint8_t addr7, uint16_t reg, uint8_t *buf, uint16_t n);
bool soft_i2c_write16(uint8_t addr7, uint16_t reg, const uint8_t *buf, uint16_t n);

#endif /* SOFT_I2C_H */
