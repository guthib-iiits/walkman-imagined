/**
  ******************************************************************************
  * @file    cs43131.c
  * @brief   Cirrus Logic CS43131 headphone DAC (x2), I2C control.
  *
  *  The audio samples arrive over SAI (see audio_out.c); this file only
  *  configures the DACs over I2C: reset, detect, clock setup, power-up,
  *  volume and mute. Register addresses are 24 bits wide, so every I2C write
  *  is 3 address bytes + 1 value byte. The sequences follow the Linux
  *  cs43130 driver and the CS43131 datasheet.
  ******************************************************************************
  */
#include "cs43131.h"
#include "walkman_config.h"

/* 24-bit register addresses */
#define DEVID_AB        0x010000U
#define DEVID_CD        0x010001U
#define DEVID_E         0x010002U
#define SYS_CLK_CTL_1   0x010006U
#define SP_SRATE        0x01000BU
#define SP_BITSIZE      0x01000CU
#define PAD_INT_CFG     0x01000DU
#define DXD1            0x010010U
#define PWDN_CTL        0x020000U
#define ASP_NUM_1       0x040010U
#define ASP_NUM_2       0x040011U
#define ASP_DEN_1       0x040012U
#define ASP_DEN_2       0x040013U
#define ASP_LRCK_HI_1   0x040014U
#define ASP_LRCK_HI_2   0x040015U
#define ASP_LRCK_PER_1  0x040016U
#define ASP_LRCK_PER_2  0x040017U
#define ASP_CLOCK_CONF  0x040018U
#define ASP_FRAME_CONF  0x040019U
#define ASP_CH_1_LOC    0x050000U
#define ASP_CH_2_LOC    0x050001U
#define ASP_CH_1_SZ_EN  0x05000AU
#define ASP_CH_2_SZ_EN  0x05000BU
#define DXD13           0x080032U
#define PCM_VOL_B       0x090001U
#define PCM_VOL_A       0x090002U
#define PCM_PATH_CTL_1  0x090003U
#define PCM_PATH_CTL_2  0x090004U

/* Bits in PWDN_CTL: 1 = that block is powered down */
#define PDN_ASP         0x40U      /* audio serial port (the I2S input) */
#define PDN_HP          0x10U      /* headphone amplifier */
#define PDN_XTAL        0x08U      /* crystal oscillator (unused: MCLK comes from the STM32) */
#define PDN_PLL         0x04U      /* internal PLL (unused) */
#define MUTE_MASK       0x03U      /* mute bits for channels A and B */

#define CS43131_ID      0x43131U   /* what the DEVID registers read on a real CS43131 */

/* Index 0 = DAC_L (SAI block A, tip), 1 = DAC_R (SAI block B, ring) */
static I2C_HandleTypeDef *const bus[CS43131_NUM_DACS] = { DAC_L_I2C, DAC_R_I2C };
static uint8_t addr[CS43131_NUM_DACS] = { CS43131_ADDR, CS43131_ADDR };
static GPIO_TypeDef *const rst_port[CS43131_NUM_DACS] = { DAC1_RST_GPIO_Port, DAC2_RST_GPIO_Port };
static const uint16_t rst_pin[CS43131_NUM_DACS] = { DAC1_RST_Pin, DAC2_RST_Pin };

/* Linux cs43130 "PCM Ch" control: 0 L-R, 1 L-L, 2 R-L, 3 R-R. The CS43131 needs
 * an extra tuning sequence whenever the right channel feeds output A. */
typedef struct { uint32_t reg; uint8_t val; } reg_seq_t;
static const reg_seq_t ch_en_seq[] = {
  { DXD1, 0x99 }, { 0x180005, 0x8C }, { 0x180007, 0xAB }, { 0x180015, 0x31 }, { 0x180017, 0xB2 },
  { 0x180025, 0x30 }, { 0x180027, 0x84 }, { 0x180035, 0x9C }, { 0x180037, 0xAE },
  { 0x18000D, 0x24 }, { 0x18000F, 0xA3 }, { 0x18001D, 0x05 }, { 0x18001F, 0xD4 },
  { 0x18002D, 0x0B }, { 0x18002F, 0xC7 }, { 0x18003D, 0x71 }, { 0x18003F, 0xE7 }, { DXD1, 0x00 },
};
static const reg_seq_t ch_dis_seq[] = {
  { DXD1, 0x99 }, { 0x180005, 0x24 }, { 0x180007, 0xA3 }, { 0x180015, 0x05 }, { 0x180017, 0xD4 },
  { 0x180025, 0x0B }, { 0x180027, 0xC7 }, { 0x180035, 0x71 }, { 0x180037, 0xE7 },
  { 0x18000D, 0x8C }, { 0x18000F, 0xAB }, { 0x18001D, 0x31 }, { 0x18001F, 0xB2 },
  { 0x18002D, 0x30 }, { 0x18002F, 0x84 }, { 0x18003D, 0x9C }, { 0x18003F, 0xAE }, { DXD1, 0x00 },
};

static uint8_t present;            /* bit d set = DAC d answered with the right ID */
static uint8_t volume = AUDIO_DEFAULT_VOLUME;
static bool muted;

/* Write one register on DAC d (0 = left, 1 = right). */
static bool wr(int d, uint32_t reg, uint8_t v)
{
  uint8_t b[4] = { (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg, v };
  return HAL_I2C_Master_Transmit(bus[d], (uint16_t)(addr[d] << 1), b, 4, 10) == HAL_OK;
}

/* Read one register: send the address, then read one byte back. */
static bool rd(int d, uint32_t reg, uint8_t *v)
{
  uint8_t b[3] = { (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg };
  if (HAL_I2C_Master_Transmit(bus[d], (uint16_t)(addr[d] << 1), b, 3, 10) != HAL_OK)
  {
    return false;
  }
  return HAL_I2C_Master_Receive(bus[d], (uint16_t)(addr[d] << 1), v, 1, 10) == HAL_OK;
}

/* Read-modify-write: change only the bits in mask, keep the others. */
static void upd(int d, uint32_t reg, uint8_t mask, uint8_t val)
{
  uint8_t v;
  if (rd(d, reg, &v))
  {
    wr(d, reg, (uint8_t)((v & ~mask) | (val & mask)));
  }
}

/* Run fn on every DAC that was found, so a missing one is skipped. */
static void each(void (*fn)(int))
{
  for (int d = 0; d < CS43131_NUM_DACS; d++)
  {
    if (present & (1U << d))
    {
      fn(d);
    }
  }
}

/* Reset both DACs, then look for each on its I2C bus by reading the chip ID.
 * Returns a bit mask of the DACs found (0b11 = both). */
uint8_t cs43131_init(void)
{
  present = 0;
  /* Hold RESET low for 2 ms, then release. */
  for (int d = 0; d < CS43131_NUM_DACS; d++)
  {
    HAL_GPIO_WritePin(rst_port[d], rst_pin[d], GPIO_PIN_RESET);
  }
  HAL_Delay(2);
  for (int d = 0; d < CS43131_NUM_DACS; d++)
  {
    HAL_GPIO_WritePin(rst_port[d], rst_pin[d], GPIO_PIN_SET);
  }
  HAL_Delay(3);

  /* Try the normal address, then the alternative (DAC_R's ADR pin floats).
   * The ID is spread over three registers: 0x43 | 0x13 | 0x1_. */
  static const uint8_t try_addr[2] = { CS43131_ADDR, CS43131_ADDR_ALT };
  for (int d = 0; d < CS43131_NUM_DACS; d++)
  {
    for (int a = 0; a < 2 && !(present & (1U << d)); a++)
    {
      uint8_t ab, cd, e;
      addr[d] = try_addr[a];
      if (rd(d, DEVID_AB, &ab) && rd(d, DEVID_CD, &cd) && rd(d, DEVID_E, &e))
      {
        uint32_t id = ((uint32_t)ab << 12) | ((uint32_t)cd << 4) | (e >> 4);
        if (id == CS43131_ID)
        {
          present |= (uint8_t)(1U << d);
          upd(d, PAD_INT_CFG, 0x01, 0x00);    /* ASP pins out of tri-state */
        }
      }
    }
  }
  return present;
}

static uint32_t cur_fs;            /* sample rate the DACs are set up for */

/* Configure DAC d for cur_fs and power it up (muted). The STM32 supplies
 * every clock (MCLK, bit clock, frame clock); the DAC just follows them. */
static void dac_start(int d)
{
  /* MCLK = 512 fs: 22.5792 MHz at 44.1 kHz, 24.576 MHz at 48 kHz (the only
   * two MCLKs the CS43131 accepts). SCLK = MCLK * 1/8 = 64 fs. */
  uint8_t mclk_22m = (cur_fs == 44100U) ? 1U : 0U;
  uint8_t num = 1, den = 8;
  uint8_t srate = mclk_22m ? 1U : 2U;             /* SP_SRATE: 1 = 44.1 k, 2 = 48 k */

  /* External MCLK, crystal and PLL off */
  upd(d, PWDN_CTL, PDN_XTAL | PDN_PLL, PDN_XTAL | PDN_PLL);
  upd(d, SYS_CLK_CTL_1, 0x07, (uint8_t)(0x00U | (mclk_22m << 2)));
  HAL_Delay(1);

  wr(d, SP_SRATE, srate);
  upd(d, SP_BITSIZE, 0x03, 0x00);                 /* 32-bit ASP */
  upd(d, ASP_CH_1_SZ_EN, 0x07, 0x07);             /* 32-bit, enabled */
  upd(d, ASP_CH_2_SZ_EN, 0x07, 0x07);

  /* I2S, 64-bit frame, CS43131 is clock slave */
  wr(d, ASP_LRCK_PER_1, 63);
  wr(d, ASP_LRCK_PER_2, 0);
  wr(d, ASP_LRCK_HI_1, 31);
  wr(d, ASP_LRCK_HI_2, 0);
  wr(d, ASP_FRAME_CONF, 0x02);
  wr(d, ASP_CH_1_LOC, 0);
  wr(d, ASP_CH_2_LOC, 32);
  wr(d, ASP_CLOCK_CONF, 0x0C);
  wr(d, ASP_DEN_1, den);
  wr(d, ASP_DEN_2, 0);
  wr(d, ASP_NUM_1, num);
  wr(d, ASP_NUM_2, 0);

#if AUDIO_DUAL_MONO
  /* Only output A is wired: DAC_L plays left on it, DAC_R plays right. */
  if (d == 0)
  {
    for (size_t i = 0; i < sizeof(ch_dis_seq) / sizeof(ch_dis_seq[0]); i++)
    {
      wr(d, ch_dis_seq[i].reg, ch_dis_seq[i].val);
    }
    upd(d, PCM_PATH_CTL_2, 0x03, 0x01);           /* Left-Left */
  }
  else
  {
    for (size_t i = 0; i < sizeof(ch_en_seq) / sizeof(ch_en_seq[0]); i++)
    {
      wr(d, ch_en_seq[i].reg, ch_en_seq[i].val);
    }
    upd(d, PCM_PATH_CTL_2, 0x03, 0x03);           /* Right-Right */
  }
#endif

  upd(d, PCM_PATH_CTL_1, MUTE_MASK, MUTE_MASK);
  upd(d, PWDN_CTL, PDN_ASP, 0);                   /* serial port on */

  /* HiFi DAC power-up with the CS43131 pop-free sequence */
  wr(d, DXD1, 0x99);
  wr(d, DXD13, 0x20);
  wr(d, DXD1, 0x00);
  upd(d, PWDN_CTL, PDN_HP, 0);
}

/* Second half of the pop-free power-up, after the settle delay. */
static void dac_post_powerup(int d)
{
  wr(d, DXD1, 0x99);
  wr(d, DXD13, 0x00);
  wr(d, DXD1, 0x00);
}

/* Mute, then power down the headphone amp and serial port. */
static void dac_stop(int d)
{
  upd(d, PCM_PATH_CTL_1, MUTE_MASK, MUTE_MASK);
  upd(d, PWDN_CTL, PDN_HP | PDN_ASP, PDN_HP | PDN_ASP);
}

/* The DAC takes attenuation, not volume: 0 = loudest, each step = -0.5 dB.
 * 100 % -> 0 dB, 50 % -> -30 dB, 1 % -> -59 dB, 0 % -> muted. */
static void dac_apply_volume(int d)
{
  uint8_t att =(volume == 0U) ? 0xFFU : (uint8_t)((100U - volume) * 6U / 5U);  /* 0.5 dB steps */
  wr(d, PCM_VOL_A, att);
  wr(d, PCM_VOL_B, att);
  upd(d, PCM_PATH_CTL_1, MUTE_MASK, (muted || volume == 0U) ? MUTE_MASK : 0U);
}

/* Start both DACs at sample rate fs (called at boot and on rate changes). */
bool cs43131_start(uint32_t fs)
{
  if (!present)
  {
    return false;
  }
  cur_fs = fs;
  each(dac_start);
  HAL_Delay(22);                                  /* 10 ms + 12 ms settle, per datasheet */
  each(dac_post_powerup);
  each(dac_apply_volume);
  return true;
}

void cs43131_stop(void)
{
  each(dac_stop);
}

void cs43131_set_volume(uint8_t percent)
{
  volume = (percent > 100U) ? 100U : percent;
  each(dac_apply_volume);
}

void cs43131_mute(bool mute)
{
  muted = mute;
  each(dac_apply_volume);
}
