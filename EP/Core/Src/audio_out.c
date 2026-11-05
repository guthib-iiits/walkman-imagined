/**
  ******************************************************************************
  * @file    audio_out.c
  * @brief   SAI1 A + B output to both CS43131s through one shared DMA buffer.
  *
  *  Both SAI blocks share the frame clock, so their DMA streams read the same
  *  circular buffer in lockstep. Each half is refilled from its start when
  *  block A finishes it; block B is at most one FIFO word behind, and that word
  *  is the last one rewritten.
  *
  *  Clocks: PLL2P = 49.152 MHz (48 k family) or 45.1584 MHz (44.1 k family).
  *  With MCK oversampling the SAI outputs MCLK = 512 fs = 24.576 / 22.5792 MHz.
  *
  *  Data path:
  *    player.c --audio_out_write()--> ring (big, filled by the main loop)
  *      --refill() in the DMA interrupt--> dma_buf (small, two halves)
  *      --DMA--> SAI1 A (left DAC) and SAI1 B (right DAC)
  *  While the DMA plays one half of dma_buf, the interrupt refills the other.
  *  If the ring runs empty, silence is sent and an "underrun" is counted.
  *
  *  The "tap" is a copy of what is being played, used by the mixtape
  *  recorder to save the song clip alongside the voice.
  ******************************************************************************
  */
#include "audio_out.h"
#include "cs43131.h"
#include "walkman_config.h"
#include <string.h>

extern SAI_HandleTypeDef hsai_BlockA1;
extern SAI_HandleTypeDef hsai_BlockB1;
extern I2S_HandleTypeDef hi2s2;

#define HALF_FRAMES   512U                      /* frames per DMA half (~10.7 ms at 48 kHz) */
#define DMA_WORDS     (HALF_FRAMES * 2U * 2U)   /* 2 halves x 2 channels (L, R) */
#define TAP_FRAMES    8192U                     /* mixtape tap buffer, ~170 ms */

/* A "frame" is one left + one right sample. Samples are 32-bit signed. */
static uint32_t dma_buf[DMA_WORDS];
static int32_t  ring[AUDIO_RING_FRAMES * 2U];
static volatile uint32_t ring_rd, ring_wr;           /* frame indices, free running */
static volatile uint32_t underruns;                  /* times the ring ran dry mid-song */
static volatile bool held;                           /* paused: send silence, keep the ring */

static int16_t  tap[TAP_FRAMES * 2U];
static volatile uint32_t tap_rd, tap_wr;
static volatile bool tap_on;

static uint32_t cur_fs = 48000U;                     /* current sample rate */
static uint8_t  cur_vol = AUDIO_DEFAULT_VOLUME;
static bool     running, suspended;                  /* SAI DMA active / powered down */

/* ---- clocks --------------------------------------------------------------- */

typedef struct { uint32_t n, frac; } pll_cfg_t;

/* HSE 25 MHz / M 5 = 5 MHz; VCO = 5 MHz * (N + FRAC/8192); output = VCO / 10 */
static const pll_cfg_t pll_48k  = { 98U, 2490U };    /* 491.520 MHz -> 49.1520 MHz */
static const pll_cfg_t pll_44k1 = { 90U, 2595U };    /* 451.584 MHz -> 45.1584 MHz */

/* 44.1 kHz (CD) and 48 kHz music need different master clocks, because
 * neither divides evenly into the other. Pick the PLL setting for fs. */
static const pll_cfg_t *family(uint32_t fs)
{
  return (fs % 11025U == 0U) ? &pll_44k1 : &pll_48k;
}

/* Retune PLL2, which clocks SAI1 (the DACs). */
static void set_pll2(const pll_cfg_t *p)
{
  RCC_PeriphCLKInitTypeDef c = {0};
  c.PeriphClockSelection = RCC_PERIPHCLK_SAI1;
  c.PLL2.PLL2M = 5;
  c.PLL2.PLL2N = p->n;
  c.PLL2.PLL2P = 10;
  c.PLL2.PLL2Q = 10;
  c.PLL2.PLL2R = 10;
  c.PLL2.PLL2RGE = RCC_PLL2VCIRANGE_2;
  c.PLL2.PLL2VCOSEL = RCC_PLL2VCOWIDE;
  c.PLL2.PLL2FRACN = p->frac;
  c.Sai1ClockSelection = RCC_SAI1CLKSOURCE_PLL2;
  HAL_RCCEx_PeriphCLKConfig(&c);
}

/* Same for PLL3, which clocks I2S2 (the microphone). The mixtape recorder
 * calls this so the mic runs at the same rate as the song. */
void audio_clock_set_i2s_family(uint32_t fs)
{
  const pll_cfg_t *p = family(fs);
  RCC_PeriphCLKInitTypeDef c = {0};
  c.PeriphClockSelection = RCC_PERIPHCLK_SPI123;
  c.PLL3.PLL3M = 5;
  c.PLL3.PLL3N = p->n;
  c.PLL3.PLL3P = 10;
  c.PLL3.PLL3Q = 10;
  c.PLL3.PLL3R = 10;
  c.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
  c.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
  c.PLL3.PLL3FRACN = p->frac;
  c.Spi123ClockSelection = RCC_SPI123CLKSOURCE_PLL3;
  HAL_RCCEx_PeriphCLKConfig(&c);
}

/* ---- SAI ------------------------------------------------------------------ */

/* (Re)initialise both SAI blocks for sample rate fs: standard I2S, 32-bit
 * samples, 2 channels. Block A is the clock master and also outputs MCLK;
 * block B follows A's clocks. This overrides what CubeMX set in main.c. */
static bool sai_config(uint32_t fs)
{
  hsai_BlockA1.Init.AudioFrequency = fs;
  hsai_BlockA1.Init.MckOverSampling = SAI_MCK_OVERSAMPLING_ENABLE;
  hsai_BlockA1.Init.MckOutput = SAI_MCK_OUTPUT_ENABLE;
  if (HAL_SAI_InitProtocol(&hsai_BlockA1, SAI_I2S_STANDARD, SAI_PROTOCOL_DATASIZE_32BIT, 2) != HAL_OK)
  {
    return false;
  }
  hsai_BlockB1.Init.AudioMode = SAI_MODESLAVE_TX;
  return HAL_SAI_InitProtocol(&hsai_BlockB1, SAI_I2S_STANDARD, SAI_PROTOCOL_DATASIZE_32BIT, 2) == HAL_OK;
}

static bool sai_start(void)
{
  memset(dma_buf, 0, sizeof(dma_buf));
  /* Sync slave first, then the master that generates the clocks. */
  if (HAL_SAI_Transmit_DMA(&hsai_BlockB1, (uint8_t *)dma_buf, DMA_WORDS) != HAL_OK)
  {
    return false;
  }
  if (HAL_SAI_Transmit_DMA(&hsai_BlockA1, (uint8_t *)dma_buf, DMA_WORDS) != HAL_OK)
  {
    HAL_SAI_DMAStop(&hsai_BlockB1);
    return false;
  }
  running = true;
  return true;
}

static void sai_stop(void)
{
  if (running)
  {
    HAL_SAI_DMAStop(&hsai_BlockA1);
    HAL_SAI_DMAStop(&hsai_BlockB1);
    running = false;
  }
}

/* Boot: start the clocks and a silent stream, then set up both DACs.
 * Returns false if the SAI or either DAC failed. */
bool audio_out_init(void)
{
  set_pll2(&pll_48k);
  cur_fs = 48000U;
  if (!sai_config(cur_fs) || !sai_start())
  {
    return false;
  }
  HAL_Delay(2);                      /* MCLK running before the DACs come out of reset */
  if (cs43131_init() == 0U)
  {
    return false;
  }
  cs43131_set_volume(cur_vol);
  return cs43131_start(cur_fs);
}

/* Switch to the sample rate of the next song. The DACs are muted while the
 * clocks change so the switch does not click. */
bool audio_out_set_rate(uint32_t fs)
{
  if (fs != 44100U && fs != 48000U)
  {
    return false;
  }
  if (fs == cur_fs && running)
  {
    return true;
  }
  cs43131_stop();
  sai_stop();
  audio_out_flush();
  if (family(fs) != family(cur_fs))
  {
    set_pll2(family(fs));
  }
  cur_fs = fs;
  if (!sai_config(fs) || !sai_start())
  {
    return false;
  }
  HAL_Delay(2);
  suspended = false;
  return cs43131_start(fs);
}

uint32_t audio_out_rate(void)
{
  return cur_fs;
}

/* Save power after a while paused: DACs off, clocks stopped. The ring is
 * kept, so audio_out_resume() carries on exactly where it stopped. */
void audio_out_suspend(void)
{
  if (suspended)
  {
    return;
  }
  cs43131_stop();
  sai_stop();
  suspended = true;
}

void audio_out_resume(void)
{
  if (!suspended)
  {
    return;
  }
  suspended = false;
  if (sai_start())
  {
    HAL_Delay(2);
    cs43131_start(cur_fs);
  }
}

bool audio_out_suspended(void)
{
  return suspended;
}

/* ---- ring buffer ---------------------------------------------------------- */

/* The ring is written only by the main loop and read only by the DMA
 * interrupt, so the two indices are enough and no lock is needed. They count
 * up forever; "% AUDIO_RING_FRAMES" turns them into a position. */

uint32_t audio_out_queued(void)
{
  return ring_wr - ring_rd;
}

uint32_t audio_out_space(void)
{
  return AUDIO_RING_FRAMES - (ring_wr - ring_rd);
}

/* Append decoded frames (L, R, L, R...). The caller checks audio_out_space()
 * first so this never overwrites unplayed audio. */
void audio_out_write(const int32_t *lr, uint32_t frames)
{
  uint32_t w = ring_wr;
  for (uint32_t i = 0; i < frames; i++)
  {
    uint32_t idx = (w % AUDIO_RING_FRAMES) * 2U;
    ring[idx] = lr[2U * i];
    ring[idx + 1U] = lr[2U * i + 1U];
    w++;
  }
  __DMB();                         /* samples must be in RAM before the index moves */
  ring_wr = w;
}

/* Drop everything queued (skip, stop, rate change). */
void audio_out_flush(void)
{
  __disable_irq();
  ring_rd = ring_wr;
  __enable_irq();
}

void audio_out_hold(bool hold)
{
  held = hold;
}

uint32_t audio_out_underruns(void)
{
  return underruns;
}

void audio_out_set_volume(uint8_t percent)
{
  cur_vol = (percent > 100U) ? 100U : percent;
  cs43131_set_volume(cur_vol);
}

uint8_t audio_out_volume(void)
{
  return cur_vol;
}

/* ---- tap ------------------------------------------------------------------ */

/* While on, refill() also copies every played frame (as 16-bit) into `tap`,
 * and the mixtape recorder drains it with audio_tap_read(). */
void audio_tap_enable(bool on)
{
  tap_rd = tap_wr;
  tap_on = on;
}

uint32_t audio_tap_read(int16_t *lr, uint32_t max_frames)
{
  uint32_t n = tap_wr - tap_rd;
  if (n > max_frames)
  {
    n = max_frames;
  }
  for (uint32_t i = 0; i < n; i++)
  {
    uint32_t idx = ((tap_rd + i) % TAP_FRAMES) * 2U;
    lr[2U * i] = tap[idx];
    lr[2U * i + 1U] = tap[idx + 1U];
  }
  tap_rd += n;
  return n;
}

/* ---- DMA interrupts ------------------------------------------------------- */

/* Runs in interrupt context: copy up to HALF_FRAMES frames from the ring into
 * one half of dma_buf, padding with silence if the ring is short. */
static void refill(uint32_t *dst)
{
  uint32_t avail = held ? 0U : ring_wr - ring_rd;
  uint32_t n = (avail < HALF_FRAMES) ? avail : HALF_FRAMES;
  uint32_t r = ring_rd;

  for (uint32_t i = 0; i < n; i++)
  {
    uint32_t idx = (r % AUDIO_RING_FRAMES) * 2U;
    dst[2U * i] = (uint32_t)ring[idx];
    dst[2U * i + 1U] = (uint32_t)ring[idx + 1U];
    r++;
  }
  if (n < HALF_FRAMES)
  {
    memset(&dst[2U * n], 0, (HALF_FRAMES - n) * 2U * sizeof(uint32_t));
    if (n > 0U)
    {
      underruns++;               /* ran dry mid-stream (silence while idle is not counted) */
    }
  }
  ring_rd = r;

  if (tap_on)
  {
    uint32_t w = tap_wr;
    for (uint32_t i = 0; i < HALF_FRAMES; i++)
    {
      uint32_t idx = (w % TAP_FRAMES) * 2U;
      tap[idx] = (int16_t)((int32_t)dst[2U * i] >> 16);
      tap[idx + 1U] = (int16_t)((int32_t)dst[2U * i + 1U] >> 16);
      w++;
    }
    tap_wr = w;
  }
}

/* HAL calls these when the DMA reaches the middle / end of dma_buf. The half
 * it just finished playing is free to refill. Only block A's interrupt is
 * used, because block B reads the same buffer in step with it. */
void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
  if (hsai == &hsai_BlockA1)
  {
    refill(&dma_buf[0]);
  }
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai)
{
  if (hsai == &hsai_BlockA1)
  {
    refill(&dma_buf[DMA_WORDS / 2U]);
  }
}
