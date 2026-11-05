/**
  ******************************************************************************
  * @file    epd.c
  * @brief   Waveshare 2.13" e-Paper (B) V3 driver (UC8151D, 104 x 212, B/W/R).
  *
  *  Colour mode follows Waveshare's EPD_2in13b_V3 code. Fast mode uses the
  *  UC8151D partial-window commands and the partial LUTs from GxEPD2's
  *  GxEPD2_213_flex driver (same controller and resolution, B/W glass).
  *
  *  Talking to the panel: SPI1 sends bytes, the DC pin says whether a byte is
  *  a command (DC low) or data (DC high), CS selects the chip, RST resets it
  *  and BUSY tells us when a refresh has finished. A refresh is started and
  *  then left to run; display.c polls epd_busy() instead of waiting here.
  *
  *  The panel has two image memories: 0x10 ("old"/black) and 0x13 ("new"/red).
  *  In colour mode they hold black and red. In fast mode they hold the old
  *  and the new black/white picture, and the waveform only moves pixels that
  *  differ between the two.
  ******************************************************************************
  */
#include "epd.h"
#include "walkman_config.h"

extern SPI_HandleTypeDef hspi1;

/* Which register setup the controller currently has loaded. Switching
 * between colour and fast mode needs a different set of commands. */
typedef enum { MODE_NONE = 0, MODE_COLOUR, MODE_FAST } epd_mode_t;

static epd_mode_t mode = MODE_NONE;
static bool asleep = true;                      /* in deep sleep: needs a reset to wake */
static uint8_t line_buf[EPD_ROW_BYTES * 8U];    /* SPI batch: 8 rows per transfer */

/* LUT = "look-up table", the voltage sequence that moves the ink particles.
 * Fast-mode LUTs (GxEPD2_213_flex "partial"). Only the first group of each
 * table is used: one phase of EPD_FAST_FRAMES frames. ww/bb drive nothing, so
 * pixels that do not change are left alone. */
#define FR EPD_FAST_FRAMES
static const uint8_t lut_vcom[44] = { 0x00, FR, 0x01, 0x00, 0x00, 0x01 };
static const uint8_t lut_ww[42]   = { 0x00, FR, 0x01, 0x00, 0x00, 0x01 };
static const uint8_t lut_bw[42]   = { 0x80, FR, 0x01, 0x00, 0x00, 0x01 };
static const uint8_t lut_wb[42]   = { 0x40, FR, 0x01, 0x00, 0x00, 0x01 };
static const uint8_t lut_bb[42]   = { 0x00, FR, 0x01, 0x00, 0x00, 0x01 };
#undef FR

static void epd_cs(GPIO_PinState s)  { HAL_GPIO_WritePin(EINK_CS_GPIO_Port, EINK_CS_Pin, s); }
static void epd_dc(GPIO_PinState s)  { HAL_GPIO_WritePin(EINK_DC_GPIO_Port, EINK_DC_Pin, s); }
static void epd_rst(GPIO_PinState s) { HAL_GPIO_WritePin(EINK_RST_GPIO_Port, EINK_RST_Pin, s); }

/* Send one command byte (DC low). */
static void epd_cmd(uint8_t c)
{
  epd_dc(GPIO_PIN_RESET);
  epd_cs(GPIO_PIN_RESET);
  HAL_SPI_Transmit(&hspi1, &c, 1, 10);
  epd_cs(GPIO_PIN_SET);
}

/* Send n parameter / pixel bytes (DC high) for the last command. */
static void epd_data(const uint8_t *d, uint16_t n)
{
  epd_dc(GPIO_PIN_SET);
  epd_cs(GPIO_PIN_RESET);
  HAL_SPI_Transmit(&hspi1, (uint8_t *)d, n, 100);
  epd_cs(GPIO_PIN_SET);
}

static void epd_data1(uint8_t d)
{
  epd_data(&d, 1);
}

/* BUSY_N is low while the controller works. Command 0x71 (get status) makes
 * the UC8151 refresh the pin, as the Waveshare reference does. */
bool epd_busy(void)
{
  epd_cmd(0x71);
  return HAL_GPIO_ReadPin(EINK_BUSY_GPIO_Port, EINK_BUSY_Pin) == GPIO_PIN_RESET;
}

/* Blocking wait for BUSY to clear. Only used for short steps (power on/off);
 * refreshes themselves are polled from display.c. */
static void epd_wait(uint32_t timeout_ms)
{
  uint32_t t0 = HAL_GetTick();
  while (epd_busy() && (HAL_GetTick() - t0) < timeout_ms)
  {
    HAL_Delay(1);
  }
}

/* Hardware reset pulse on RST. Also wakes the panel from deep sleep and
 * clears every register, so the mode has to be set up again afterwards. */
static void epd_reset(void)
{
  epd_rst(GPIO_PIN_SET);
  HAL_Delay(10);
  epd_rst(GPIO_PIN_RESET);
  HAL_Delay(2);
  epd_rst(GPIO_PIN_SET);
  HAL_Delay(10);
  asleep = false;
  mode = MODE_NONE;
}

static void resolution(void)
{
  epd_cmd(0x61);
  epd_data1(0x68);            /* 104 */
  epd_data1(0x00);
  epd_data1(0xD4);            /* 212 */
}

/* Set up for a full black/white/red refresh using the waveform stored in the
 * panel's OTP memory by the factory (slow, ~15 s, but correct colours). */
static void enter_colour_mode(void)
{
  if (asleep || mode == MODE_FAST)
  {
    epd_reset();              /* back to the power-on register defaults the OTP waveform expects */
  }
  if (mode == MODE_COLOUR)
  {
    return;
  }
  epd_cmd(0x04);              /* power on */
  epd_wait(500);
  epd_cmd(0x00);              /* panel setting: LUT from OTP, KWR mode */
  epd_data1(0x0F);
  epd_data1(0x89);
  resolution();
  epd_cmd(0x50);              /* VCOM and data interval */
  epd_data1(0x77);
  mode = MODE_COLOUR;
}

/* Set up for fast black/white partial updates: our own LUTs (above) are
 * loaded into the controller instead of the factory waveform. */
static void enter_fast_mode(void)
{
  if (asleep)
  {
    epd_reset();
  }
  if (mode == MODE_FAST)
  {
    return;
  }
  epd_cmd(0x01);              /* power setting */
  epd_data1(0x03);
  epd_data1(0x00);
  epd_data1(0x2B);
  epd_data1(0x2B);
  epd_data1(0x03);
  epd_cmd(0x06);              /* booster soft start */
  epd_data1(0x17);
  epd_data1(0x17);
  epd_data1(0x17);
  epd_cmd(0x00);              /* panel setting: LUT from register, KW mode */
  epd_data1(0xBF);
  epd_data1(0x0D);
  epd_cmd(0x30);              /* PLL: 100 Hz frame rate */
  epd_data1(0x3A);
  resolution();
  epd_cmd(0x82);              /* VCOM DC */
  epd_data1(0x08);
  epd_cmd(0x50);
  epd_data1(0x17);
  epd_cmd(0x20); epd_data(lut_vcom, sizeof(lut_vcom));
  epd_cmd(0x21); epd_data(lut_ww, sizeof(lut_ww));
  epd_cmd(0x22); epd_data(lut_bw, sizeof(lut_bw));
  epd_cmd(0x23); epd_data(lut_wb, sizeof(lut_wb));
  epd_cmd(0x24); epd_data(lut_bb, sizeof(lut_bb));
  epd_cmd(0x04);
  epd_wait(500);
  mode = MODE_FAST;
}

void epd_init(void)
{
  epd_reset();
}

/* Send a rectangle of one plane to image memory `cmd` (0x10 or 0x13).
 * x is in bytes (8 pixels each), y in rows.
 * The controller wants 1 = white, 0 = ink, so planes are inverted on the way out. */
static void send_window(uint8_t cmd, const uint8_t *plane, uint8_t xb0, uint8_t xb1, uint16_t y0, uint16_t y1)
{
  uint32_t w = (uint32_t)(xb1 - xb0 + 1U);
  uint32_t n = 0;
  epd_cmd(cmd);
  for (uint16_t y = y0; y <= y1; y++)
  {
    for (uint32_t x = 0; x < w; x++)
    {
      line_buf[n++] = (uint8_t)~plane[y * EPD_ROW_BYTES + xb0 + x];
      if (n == sizeof(line_buf))
      {
        epd_data(line_buf, (uint16_t)n);
        n = 0;
      }
    }
  }
  if (n)
  {
    epd_data(line_buf, (uint16_t)n);
  }
}

/* Start a full colour refresh and return straight away (BUSY stays low for
 * ~15 s). k = black plane, r = red plane, whole screen. */
void epd_colour_start(const uint8_t *k, const uint8_t *r)
{
  enter_colour_mode();
  send_window(0x10, k, 0, EPD_ROW_BYTES - 1U, 0, EPD_HEIGHT - 1U);
  send_window(0x13, r, 0, EPD_ROW_BYTES - 1U, 0, EPD_HEIGHT - 1U);
  epd_cmd(0x12);              /* display refresh */
  HAL_Delay(1);
}

void epd_fast_start(const uint8_t *old_k, const uint8_t *new_k,
                    uint8_t xb0, uint8_t xb1, uint16_t y0, uint16_t y1)
{
  enter_fast_mode();
  epd_cmd(0x91);              /* partial in */
  epd_cmd(0x90);              /* partial window */
  epd_data1((uint8_t)(xb0 * 8U));          /* x start (pixels) */
  epd_data1((uint8_t)(xb1 * 8U + 7U));     /* x end */
  epd_data1((uint8_t)(y0 >> 8));           /* y start, high then low byte */
  epd_data1((uint8_t)y0);
  epd_data1((uint8_t)(y1 >> 8));           /* y end */
  epd_data1((uint8_t)y1);
  epd_data1(0x01);                         /* scan only inside the window */
  send_window(0x10, old_k, xb0, xb1, y0, y1);
  send_window(0x13, new_k, xb0, xb1, y0, y1);
  epd_cmd(0x12);
  HAL_Delay(1);
}

void epd_fast_finish(void)
{
  epd_cmd(0x92);              /* partial out, once BUSY has cleared */
}

/* Stop a running refresh by resetting the controller. The glass is left
 * half-updated, which display.c handles by redrawing everything. */
void epd_abort(void)
{
  epd_reset();
}

/* Power the panel down. The picture stays on the glass with no power. */
void epd_sleep(void)
{
  if (asleep)
  {
    return;
  }
  epd_wait(20000);            /* let a running refresh finish first */
  epd_cmd(0x50);              /* border floating, so the edge is not driven while off */
  epd_data1(0xF7);
  epd_cmd(0x02);              /* power off */
  epd_wait(1000);
  epd_cmd(0x07);              /* deep sleep */
  epd_data1(0xA5);
  asleep = true;
  mode = MODE_NONE;
}
