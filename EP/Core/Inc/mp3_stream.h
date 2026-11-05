/**
  ******************************************************************************
  * @file    mp3_stream.h
  * @brief   MP3 file decoder (minimp3) with ID3v2 tags, duration and seeking.
 *          Also plays 16-bit PCM WAV, the format mixtapes are recorded in.
  *          Output is 32-bit stereo at 32 / 44.1 / 48 kHz; low-rate MPEG-2/2.5
  *          files are upsampled by 2x or 4x so the DACs always see a native rate.
  ******************************************************************************
  */
#ifndef MP3_STREAM_H
#define MP3_STREAM_H

#include <stdint.h>
#include <stdbool.h>
#include "ff.h"
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_ONLY_MP3
#ifndef MINIMP3_NO_SIMD
#define MINIMP3_NO_SIMD            /* Cortex-M7 has no NEON */
#endif
#include "minimp3.h"

#define MP3S_MAX_OUT_FRAMES (1152U * 4U)   /* worst MP3 case: 576-sample 8 kHz frame x 6 = 3456 */
#define MP3S_INBUF          16384U

typedef struct {
  char title[64];
  char artist[48];
  char album[48];
  uint32_t art_offset;          /* embedded JPEG (APIC), 0 if none */
  uint32_t art_size;
} mp3_meta_t;

typedef struct {
  FIL f;
  bool open;
  bool is_wav;                  /* 16-bit PCM WAV (mixtape recordings) */
  uint16_t wav_channels;
  mp3dec_t dec;
  uint8_t in[MP3S_INBUF];
  uint32_t in_len, in_pos;
  bool eof;
  uint32_t audio_start, file_size;
  uint32_t src_rate, out_rate;  /* file rate / rate sent to the DACs (44100 or 48000) */
  uint32_t acc;                 /* resampler phase, see emit() */
  uint32_t bitrate_kbps;
  uint32_t duration_ms;
  uint8_t toc[100];
  bool has_toc;
  uint64_t frames_out;          /* output frames since the start of the track */
  int32_t last_l, last_r;       /* resampler: previous input frame */
  bool full_precision;
  mp3_meta_t meta;
} mp3s_t;

bool     mp3s_open(mp3s_t *s, const char *path, bool full_precision);
void     mp3s_close(mp3s_t *s);
/* Decodes one MP3 frame into out (interleaved L/R). Returns frames written,
 * 0 at end of file. out must hold MP3S_MAX_OUT_FRAMES frames. */
uint32_t mp3s_decode(mp3s_t *s, int32_t *out);
void     mp3s_seek(mp3s_t *s, uint32_t ms);
uint32_t mp3s_position_ms(const mp3s_t *s);

#endif /* MP3_STREAM_H */
