/**
  ******************************************************************************
  * @file    mic_in.c
  * @brief   16-bit I2S microphone on I2S2 (master RX, circular DMA).
  *
  *  Same double-buffer idea as audio_out.c, in the other direction: DMA fills
  *  one half of dma_buf while the interrupt copies the other half into a
  *  ring, which mixtape.c drains from the main loop. The INMP441 only sends
  *  on one channel (left or right slot), so only that slot is kept (mono).
  ******************************************************************************
  */
#include "mic_in.h"
#include "audio_out.h"
#include "walkman_config.h"

extern I2S_HandleTypeDef hi2s2;

#define MIC_HALF_FRAMES  256U      /* frames per DMA half */
#define MIC_RING         16384U    /* mono samples, ~0.34 s at 48 kHz */

static uint16_t dma_buf[MIC_HALF_FRAMES * 2U * 2U];    /* L,R halfwords, two halves */
static int16_t  ring[MIC_RING];
static volatile uint32_t rd, wr;          /* free-running sample indices */
static volatile uint16_t peak;            /* loudest sample since last mic_peak() (level meter) */
static bool running;

/* Start recording at sample rate fs (matched to the song being played). */
bool mic_start(uint32_t fs)
{
  mic_stop();
  audio_clock_set_i2s_family(fs);
  hi2s2.Init.AudioFreq = fs;
  if (HAL_I2S_Init(&hi2s2) != HAL_OK)
  {
    return false;
  }
  rd = wr = 0;
  if (HAL_I2S_Receive_DMA(&hi2s2, dma_buf, (uint16_t)(sizeof(dma_buf) / sizeof(dma_buf[0]))) != HAL_OK)
  {
    return false;
  }
  running = true;
  return true;
}

void mic_stop(void)
{
  if (running)
  {
    HAL_I2S_DMAStop(&hi2s2);
    running = false;
  }
}

/* Copy up to max_samples recorded samples out; returns how many. */
uint32_t mic_read(int16_t *mono, uint32_t max_samples)
{
  uint32_t n = wr - rd;
  if (n > max_samples)
  {
    n = max_samples;
  }
  for (uint32_t i = 0; i < n; i++)
  {
    mono[i] = ring[(rd + i) % MIC_RING];
  }
  rd += n;
  return n;
}

void mic_discard(void)
{
  rd = wr;
}

uint16_t mic_peak(void)
{
  uint16_t p = peak;
  peak = 0;
  return p;
}

/* Interrupt context: move one finished DMA half into the ring and track the
 * peak level. */
static void push(const uint16_t *src)
{
  uint32_t w = wr;
  uint16_t pk = peak;
  for (uint32_t i = 0; i < MIC_HALF_FRAMES; i++)
  {
    int16_t s = (int16_t)src[2U * i + (MIC_USE_LEFT_SLOT ? 0U : 1U)];
    uint16_t a = (uint16_t)(s < 0 ? -s : s);
    if (a > pk)
    {
      pk = a;
    }
    ring[w % MIC_RING] = s;
    w++;
  }
  if (w - rd > MIC_RING)
  {
    rd = w - MIC_RING;          /* consumer too slow: keep the newest audio */
  }
  wr = w;
  peak = pk;
}

void HAL_I2S_RxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
  if (hi2s == &hi2s2)
  {
    push(&dma_buf[0]);
  }
}

void HAL_I2S_RxCpltCallback(I2S_HandleTypeDef *hi2s)
{
  if (hi2s == &hi2s2)
  {
    push(&dma_buf[MIC_HALF_FRAMES * 2U]);
  }
}
