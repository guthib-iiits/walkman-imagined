/**
  ******************************************************************************
  * @file    soft_i2c.c
  * @brief   Bit-banged I2C master (~100 kHz) on open-drain GPIOs.
  *          The 10 k pull-ups to VDDIO sit next to the nPM1300.
  *
  *  PE12/PE13 have no I2C hardware, so the protocol is done in software by
  *  toggling the pins. "Open drain" means a pin can only pull the line low;
  *  writing "high" just lets go and the pull-up resistor raises it. That is
  *  how I2C works, and it lets the PMIC pull the lines low too (ACK bits,
  *  clock stretching).
  ******************************************************************************
  */
#include "soft_i2c.h"
#include "walkman_config.h"

/* BSRR sets (low 16 bits) or clears (high 16 bits) a pin in one write;
 * IDR reads the actual level on the pin. */
#define SDA_HI()  (PMIC_SDA_PORT->BSRR = PMIC_SDA_PIN)
#define SDA_LO()  (PMIC_SDA_PORT->BSRR = (uint32_t)PMIC_SDA_PIN << 16)
#define SCL_HI()  (PMIC_SCL_PORT->BSRR = PMIC_SCL_PIN)
#define SCL_LO()  (PMIC_SCL_PORT->BSRR = (uint32_t)PMIC_SCL_PIN << 16)
#define SDA_IN()  ((PMIC_SDA_PORT->IDR & PMIC_SDA_PIN) != 0U)
#define SCL_IN()  ((PMIC_SCL_PORT->IDR & PMIC_SCL_PIN) != 0U)

static void delay_half(void)
{
  /* ~5 us at 400 MHz: 100 kHz bus */
  for (volatile uint32_t i = 0; i < 400U; i++)
  {
  }
}

/* Let SCL go high, and wait if the slave is holding it low ("stretching")
 * because it needs more time. */
static void scl_release(void)
{
  SCL_HI();
  for (uint32_t t = 0; t < 1000U && !SCL_IN(); t++)   /* clock stretching */
  {
    delay_half();
  }
}

void soft_i2c_init(void)
{
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_GPIOE_CLK_ENABLE();
  SDA_HI();
  SCL_HI();
  g.Pin = PMIC_SDA_PIN | PMIC_SCL_PIN;
  g.Mode = GPIO_MODE_OUTPUT_OD;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(PMIC_SDA_PORT, &g);

  /* Free a slave stuck mid-byte after a reset */
  for (int i = 0; i < 9 && !SDA_IN(); i++)
  {
    SCL_LO();
    delay_half();
    scl_release();
    delay_half();
  }
}

/* START condition: SDA falls while SCL is high. */
static void start(void)
{
  SDA_HI();
  scl_release();
  delay_half();
  SDA_LO();
  delay_half();
  SCL_LO();
}

/* STOP condition: SDA rises while SCL is high. */
static void stop(void)
{
  SDA_LO();
  delay_half();
  scl_release();
  delay_half();
  SDA_HI();
  delay_half();
}

/* Clock out 8 bits, most significant first, then read the slave's ACK
 * (slave pulls SDA low = "got it"). Returns true on ACK. */
static bool write_byte(uint8_t b)
{
  for (int i = 0; i < 8; i++)
  {
    if (b & 0x80U)
    {
      SDA_HI();
    }
    else
    {
      SDA_LO();
    }
    b <<= 1;
    delay_half();
    scl_release();
    delay_half();
    SCL_LO();
  }
  SDA_HI();
  delay_half();
  scl_release();
  bool ack = !SDA_IN();
  delay_half();
  SCL_LO();
  return ack;
}

/* Clock in 8 bits. ack = true asks the slave for another byte; the last
 * byte is answered with NACK so the slave stops sending. */
static uint8_t read_byte(bool ack)
{
  uint8_t b = 0;
  SDA_HI();
  for (int i = 0; i < 8; i++)
  {
    delay_half();
    scl_release();
    b = (uint8_t)((b << 1) | (SDA_IN() ? 1U : 0U));
    delay_half();
    SCL_LO();
  }
  if (ack)
  {
    SDA_LO();
  }
  delay_half();
  scl_release();
  delay_half();
  SCL_LO();
  SDA_HI();
  return b;
}

/* Write n bytes to a 16-bit register address: [addr+W][reg hi][reg lo][data...] */
bool soft_i2c_write16(uint8_t addr7, uint16_t reg, const uint8_t *buf, uint16_t n)
{
  start();
  bool ok = write_byte((uint8_t)(addr7 << 1)) && write_byte((uint8_t)(reg >> 8)) && write_byte((uint8_t)reg);
  for (uint16_t i = 0; ok && i < n; i++)
  {
    ok = write_byte(buf[i]);
  }
  stop();
  return ok;
}

/* Read n bytes: write the register address, then restart in read mode:
 * [addr+W][reg hi][reg lo] [restart][addr+R][data...] */
bool soft_i2c_read16(uint8_t addr7, uint16_t reg, uint8_t *buf, uint16_t n)
{
  start();
  bool ok = write_byte((uint8_t)(addr7 << 1)) && write_byte((uint8_t)(reg >> 8)) && write_byte((uint8_t)reg);
  if (ok)
  {
    start();                                   /* repeated start */
    ok = write_byte((uint8_t)((addr7 << 1) | 1U));
    for (uint16_t i = 0; ok && i < n; i++)
    {
      buf[i] = read_byte(i + 1U < n);
    }
  }
  stop();
  return ok;
}
