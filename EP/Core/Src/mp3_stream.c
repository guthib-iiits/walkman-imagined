/**
  ******************************************************************************
  * @file    mp3_stream.c
  * @brief   MP3 file decoder (minimp3) with ID3v2 tags, duration and seeking.
  *
  *  Also plays 16-bit PCM .wav files through the same interface. Whatever
  *  the file, mp3s_decode() hands back stereo 32-bit frames at out_rate.
  *
  *  An MP3 file is: [ID3 tag: title, artist, cover picture] then a long run
  *  of "frames", each holding 1152 samples (~24 ms) of compressed audio.
  *  The first frame may be a Xing/Info header with the song length and a
  *  seek table instead of audio.
  *
  *  Every file is resampled to 44.1 kHz (the 11025 Hz family) or 48 kHz
  *  (everything else), the two rates the CS43131 can run from the 22.5792 /
  *  24.576 MHz MCLK the SAI generates. Most files are already at one of them
  *  and pass straight through.
  ******************************************************************************
  */
#define MINIMP3_IMPLEMENTATION      /* compile the minimp3 decoder into this file */
#include "mp3_stream.h"
#include <string.h>

static float pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];   /* one decoded frame, -1.0..1.0 */

/* ---- input buffer ---------------------------------------------------------- */

/* Keep s->in topped up from the file: shift the unread bytes to the front,
 * then read more behind them. */
static void refill(mp3s_t *s)
{
  if (s->in_pos > 0U)
  {
    memmove(s->in, s->in + s->in_pos, s->in_len - s->in_pos);
    s->in_len -= s->in_pos;
    s->in_pos = 0;
  }
  if (!s->eof && s->in_len < MP3S_INBUF)
  {
    UINT got = 0;
    if (f_read(&s->f, s->in + s->in_len, MP3S_INBUF - s->in_len, &got) != FR_OK || got == 0U)
    {
      s->eof = true;
    }
    s->in_len += got;
  }
}

/* ---- ID3v2 ------------------------------------------------------------------ */

/* ID3 sizes use 7 bits per byte ("syncsafe") so they never look like an
 * MP3 frame sync pattern. */
static uint32_t syncsafe(const uint8_t *p)
{
  return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
         ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

/* ID3 text frames to plain ASCII (the font has no accents anyway). */
static void id3_text(char *dst, size_t cap, const uint8_t *src, uint32_t len)
{
  size_t o = 0;
  if (len == 0U)
  {
    return;
  }
  uint8_t enc = src[0];
  src++;
  len--;
  bool utf16 = (enc == 1U || enc == 2U);
  bool be = (enc == 2U);
  if (enc == 1U && len >= 2U)
  {
    be = (src[0] == 0xFE);
    src += 2;
    len -= 2;
  }
  for (uint32_t i = 0; i < len && o + 1U < cap;)
  {
    uint32_t c;
    if (utf16)
    {
      if (i + 1U >= len)
      {
        break;
      }
      c = be ? ((uint32_t)src[i] << 8 | src[i + 1U]) : ((uint32_t)src[i + 1U] << 8 | src[i]);
      i += 2U;
    }
    else
    {
      c = src[i++];
      if (enc == 3U && c >= 0x80U)       /* UTF-8 continuation: collapse to one '?' */
      {
        while (i < len && (src[i] & 0xC0U) == 0x80U)
        {
          i++;
        }
      }
    }
    if (c == 0U)
    {
      break;
    }
    dst[o++] = (c >= 32U && c < 127U) ? (char)c : '?';
  }
  dst[o] = '\0';
}

/* Read the ID3v2 tag at the start of the file: title (TIT2), artist (TPE1),
 * album (TALB) and where the cover JPEG is (APIC). Only the part of the tag
 * already in the input buffer is parsed. Sets audio_start past the tag. */
static void parse_id3(mp3s_t *s)
{
  uint8_t *h = s->in;
  if (s->in_len < 10U || memcmp(h, "ID3", 3) != 0)
  {
    return;
  }
  uint8_t ver = h[3];
  uint32_t tag_size = syncsafe(&h[6]) + 10U + ((h[5] & 0x10U) ? 10U : 0U);
  s->audio_start = tag_size;

  uint32_t p = 10;
  if (h[5] & 0x40U)                                  /* extended header */
  {
    p += (ver == 4U) ? syncsafe(&h[10]) : (((uint32_t)h[10] << 24 | (uint32_t)h[11] << 16 |
                                            (uint32_t)h[12] << 8 | h[13]) + 4U);
  }
  /* Walk the tag's frames: 4-char ID, 4-byte size, 2 flag bytes, then data. */
  while (p + 10U <= tag_size && p + 10U <= s->in_len)
  {
    uint8_t *fr = &h[p];
    if (fr[0] == 0U)
    {
      break;
    }
    uint32_t sz = (ver == 4U) ? syncsafe(&fr[4])
                              : ((uint32_t)fr[4] << 24 | (uint32_t)fr[5] << 16 | (uint32_t)fr[6] << 8 | fr[7]);
    uint32_t body = p + 10U;
    uint32_t avail = (body + sz <= s->in_len) ? sz : (s->in_len > body ? s->in_len - body : 0U);

    if (memcmp(fr, "TIT2", 4) == 0)
    {
      id3_text(s->meta.title, sizeof(s->meta.title), &h[body], avail);
    }
    else if (memcmp(fr, "TPE1", 4) == 0)
    {
      id3_text(s->meta.artist, sizeof(s->meta.artist), &h[body], avail);
    }
    else if (memcmp(fr, "TALB", 4) == 0)
    {
      id3_text(s->meta.album, sizeof(s->meta.album), &h[body], avail);
    }
    else if (memcmp(fr, "APIC", 4) == 0 && avail > 16U)
    {
      /* enc, mime\0, type, description\0 (terminator width depends on enc), data */
      uint32_t q = body + 1U;
      const char *mime = (const char *)&h[q];
      bool jpeg = (strstr(mime, "jpeg") != NULL) || (strstr(mime, "jpg") != NULL);
      while (q < body + avail && h[q] != 0U)
      {
        q++;
      }
      q += 2U;                                         /* NUL + picture type */
      if (h[body] == 1U || h[body] == 2U)
      {
        while (q + 1U < body + avail && (h[q] != 0U || h[q + 1U] != 0U))
        {
          q += 2U;
        }
        q += 2U;
      }
      else
      {
        while (q < body + avail && h[q] != 0U)
        {
          q++;
        }
        q++;
      }
      if (jpeg && q < body + sz && s->meta.art_offset == 0U)
      {
        s->meta.art_offset = q;
        s->meta.art_size = body + sz - q;
      }
    }
    p = body + sz;
  }
}

/* ---- Xing / Info header ----------------------------------------------------- */

/* VBR files carry a Xing/Info header in the first frame: the total frame
 * count (-> exact duration) and a 100-entry table of contents for seeking
 * (toc[i] = file position of i % of the song, scaled to 0..255). */
static void parse_xing(mp3s_t *s, const uint8_t *frame, uint32_t len, uint32_t samples_per_frame)
{
  for (uint32_t i = 4; i + 12U < len && i < 64U; i++)
  {
    if (memcmp(&frame[i], "Xing", 4) == 0 || memcmp(&frame[i], "Info", 4) == 0)
    {
      const uint8_t *p = &frame[i + 4U];
      uint32_t flags = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
      p += 4;
      if (flags & 1U)
      {
        uint32_t frames = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        s->duration_ms = (uint32_t)((uint64_t)frames * samples_per_frame * 1000U / s->src_rate);
        p += 4;
      }
      if (flags & 2U)
      {
        p += 4;
      }
      if ((flags & 4U) && (p + 100U) <= frame + len)
      {
        memcpy(s->toc, p, 100);
        s->has_toc = true;
      }
      return;
    }
  }
}

/* ---- WAV -------------------------------------------------------------------- */

/* Output rate for a file rate: 44.1 kHz for 11025 / 22050 / 44100 / 88200,
 * 48 kHz for everything else (8k ... 96k). */
static uint32_t out_rate_for(uint32_t src_rate)
{
  return (src_rate % 11025U == 0U) ? 44100U : 48000U;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* A WAV file is a list of "chunks": "fmt " says rate / channels / bits, and
 * "data" holds the samples. Only 16-bit PCM, mono or stereo, is accepted. */
static bool open_wav(mp3s_t *s)
{
  uint32_t p = 12;
  uint16_t fmt = 0, bits = 0;
  while (p + 8U <= s->in_len)
  {
    uint32_t sz = le32(&s->in[p + 4U]);
    if (memcmp(&s->in[p], "fmt ", 4) == 0 && p + 24U <= s->in_len)
    {
      fmt = le16(&s->in[p + 8U]);
      s->wav_channels = le16(&s->in[p + 10U]);
      s->src_rate = le32(&s->in[p + 12U]);
      bits = le16(&s->in[p + 22U]);
    }
    else if (memcmp(&s->in[p], "data", 4) == 0)
    {
      s->audio_start = p + 8U;
      break;
    }
    p += 8U + sz + (sz & 1U);
  }
  if (fmt != 1U || bits != 16U || s->wav_channels == 0U || s->wav_channels > 2U || s->audio_start == 0U)
  {
    return false;
  }
  s->is_wav = true;
  s->out_rate = out_rate_for(s->src_rate);
  s->acc = s->src_rate;
  uint32_t bytes_per_s = s->src_rate * 2U * s->wav_channels;
  s->duration_ms = (uint32_t)((uint64_t)(s->file_size - s->audio_start) * 1000U / bytes_per_s);
  s->in_pos = s->audio_start;
  return true;
}

/* ---- public ----------------------------------------------------------------- */

/* No title tag: use the file name without ".mp3". */
static void title_from_path(mp3s_t *s, const char *path)
{
  if (s->meta.title[0] != '\0')
  {
    return;
  }
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  strncpy(s->meta.title, name, sizeof(s->meta.title) - 1);
  char *dot = strrchr(s->meta.title, '.');
  if (dot)
  {
    *dot = '\0';
  }
}

/* Open a song and read everything needed before playing: tags, sample
 * rate, bitrate, duration. full_precision = Settings -> Audio Quality 32-bit.
 * Returns false if the file is missing or not a playable MP3 / WAV. */
bool mp3s_open(mp3s_t *s, const char *path, bool full_precision)
{
  memset(&s->meta, 0, sizeof(s->meta));
  s->open = false;
  s->is_wav = false;
  if (f_open(&s->f, path, FA_READ) != FR_OK)
  {
    return false;
  }
  s->open = true;
  s->file_size = (uint32_t)f_size(&s->f);
  s->in_len = s->in_pos = 0;
  s->eof = false;
  s->audio_start = 0;
  s->has_toc = false;
  s->duration_ms = 0;
  s->frames_out = 0;
  s->last_l = s->last_r = 0;
  s->full_precision = full_precision;
  mp3dec_init(&s->dec);

  refill(s);
  if (s->in_len >= 12U && memcmp(s->in, "RIFF", 4) == 0 && memcmp(&s->in[8], "WAVE", 4) == 0)
  {
    if (!open_wav(s))
    {
      mp3s_close(s);
      return false;
    }
    title_from_path(s, path);
    return true;
  }
  parse_id3(s);
  title_from_path(s, path);

  /* Jump past the tag, then find the first real frame. */
  if (s->audio_start >= s->in_len)
  {
    f_lseek(&s->f, s->audio_start);
    s->in_len = s->in_pos = 0;
    s->eof = false;
    refill(s);
  }
  else
  {
    s->in_pos = s->audio_start;
    refill(s);
  }

  /* Decode (and throw away) up to 8 frames to find the first real audio
   * frame and learn the sample rate and bitrate from it. */
  mp3dec_frame_info_t info;
  int samples = 0;
  for (int tries = 0; tries < 8 && samples == 0; tries++)
  {
    samples = mp3dec_decode_frame(&s->dec, s->in, (int)s->in_len, NULL, &info);
    if (info.frame_bytes == 0)
    {
      break;
    }
    if (samples == 0)
    {
      s->in_pos = (uint32_t)(info.frame_offset + info.frame_bytes);
      s->audio_start += s->in_pos;
      refill(s);
    }
  }
  if (samples == 0 || info.hz == 0)
  {
    mp3s_close(s);
    return false;
  }
  s->audio_start += (uint32_t)info.frame_offset;
  s->src_rate = (uint32_t)info.hz;
  s->out_rate = out_rate_for(s->src_rate);
  s->acc = s->src_rate;
  s->bitrate_kbps = (uint32_t)info.bitrate_kbps;

  parse_xing(s, s->in + info.frame_offset, (uint32_t)info.frame_bytes, (uint32_t)samples);
  if (s->duration_ms == 0U && s->bitrate_kbps)       /* no Xing: assume constant bitrate */
  {
    s->duration_ms = (uint32_t)((uint64_t)(s->file_size - s->audio_start) * 8U / s->bitrate_kbps);
  }

  /* The probe above only parsed headers; restart cleanly at the first frame. */
  mp3dec_init(&s->dec);
  return true;
}

void mp3s_close(mp3s_t *s)
{
  if (s->open)
  {
    f_close(&s->f);
    s->open = false;
  }
}

/* Decoder output (-1.0..1.0 float) to the 32-bit samples the DACs take.
 * In 16-bit mode the low 16 bits are zeroed. */
static inline int32_t to_s32(float v, bool full)
{
  if (v >= 1.0f)
  {
    return INT32_MAX;
  }
  if (v <= -1.0f)
  {
    return INT32_MIN;
  }
  if (full)
  {
    return (int32_t)(v * 2147483647.0f);
  }
  return (int32_t)((int16_t)(v * 32767.0f)) << 16;     /* 16-bit "Audio Quality" */
}

/* Takes one source frame and writes the output frames that fall between the
 * previous source frame and this one (none, one or several), linearly
 * interpolated. acc is where the next output sits in that gap, in units of
 * 1/out_rate of a source frame, so any ratio comes out exact. */
static uint32_t emit(mp3s_t *s, int32_t *out, uint32_t o, int32_t l, int32_t r)
{
  if (s->src_rate == s->out_rate)    /* the usual case: no resampling */
  {
    out[2U * o] = l;
    out[2U * o + 1U] = r;
    return o + 1U;
  }
  while (s->acc <= s->out_rate)
  {
    int64_t a = (int64_t)s->acc, b = (int64_t)s->out_rate - a;
    out[2U * o] = (int32_t)(((int64_t)s->last_l * b + (int64_t)l * a) / (int64_t)s->out_rate);
    out[2U * o + 1U] = (int32_t)(((int64_t)s->last_r * b + (int64_t)r * a) / (int64_t)s->out_rate);
    o++;
    s->acc += s->src_rate;
  }
  s->acc -= s->out_rate;
  s->last_l = l;
  s->last_r = r;
  return o;
}

/* WAV "decoding": just convert up to 1152 frames of 16-bit samples (fewer
 * for low rates, so the resampled output still fits in `out`). */
static uint32_t decode_wav(mp3s_t *s, int32_t *out)
{
  uint32_t fb = 2U * s->wav_channels;
  uint32_t max = (MP3S_MAX_OUT_FRAMES - 1U) * s->src_rate / s->out_rate;
  if (max > 1152U)
  {
    max = 1152U;
  }
  if (s->in_len - s->in_pos < max * fb)
  {
    refill(s);
  }
  uint32_t n = (s->in_len - s->in_pos) / fb;
  if (n > max)
  {
    n = max;
  }
  uint32_t o = 0;
  for (uint32_t i = 0; i < n; i++)
  {
    const uint8_t *p = &s->in[s->in_pos + i * fb];
    int32_t l = (int32_t)(int16_t)le16(p) << 16;
    int32_t r = (s->wav_channels > 1U) ? (int32_t)(int16_t)le16(p + 2) << 16 : l;
    o = emit(s, out, o, l, r);
  }
  s->in_pos += n * fb;
  s->frames_out += o;
  return o;
}

/* Decode the next MP3 frame into `out` (stereo, interleaved). Returns the
 * number of frames written (at most MP3S_MAX_OUT_FRAMES); 0 = end of song. */
uint32_t mp3s_decode(mp3s_t *s, int32_t *out)
{
  mp3dec_frame_info_t info;

  if (s->open && s->is_wav)
  {
    return decode_wav(s, out);
  }
  while (s->open)
  {
    if (s->in_len - s->in_pos < 4096U)
    {
      refill(s);
    }
    if (s->in_len == s->in_pos)
    {
      return 0;
    }
    int n = mp3dec_decode_frame(&s->dec, s->in + s->in_pos, (int)(s->in_len - s->in_pos), pcm, &info);
    if (info.frame_bytes == 0)
    {
      if (s->eof)
      {
        return 0;
      }
      s->in_pos = s->in_len;            /* garbage: drop it and read more */
      continue;
    }
    s->in_pos += (uint32_t)info.frame_bytes;
    if (n == 0)
    {
      continue;                         /* skipped data or still syncing */
    }

    uint32_t o = 0;
    for (int i = 0; i < n; i++)
    {
      int32_t l = to_s32(pcm[i * info.channels], s->full_precision);
      int32_t r = (info.channels > 1) ? to_s32(pcm[i * info.channels + 1], s->full_precision) : l;
      o = emit(s, out, o, l, r);
    }
    s->frames_out += o;
    return o;
  }
  return 0;
}

/* Jump to `ms` into the song. MP3 frames don't carry time stamps, so the
 * byte position is estimated: from the Xing table if there is one,
 * otherwise in proportion to the file size. The decoder resyncs at the
 * next frame it finds. */
void mp3s_seek(mp3s_t *s, uint32_t ms)
{
  if (!s->open || s->duration_ms == 0U)
  {
    return;
  }
  if (ms >= s->duration_ms)
  {
    ms = s->duration_ms - 1U;
  }
  uint32_t audio_bytes = s->file_size - s->audio_start;
  uint32_t off;
  if (s->is_wav)
  {
    uint32_t fb = 2U * s->wav_channels;
    off = (uint32_t)((uint64_t)ms * s->src_rate / 1000U) * fb;
  }
  else if (s->has_toc)
  {
    uint32_t pct = (uint32_t)((uint64_t)ms * 100U / s->duration_ms);
    off = (uint32_t)((uint64_t)s->toc[pct > 99U ? 99U : pct] * audio_bytes / 256U);
  }
  else
  {
    off = (uint32_t)((uint64_t)ms * audio_bytes / s->duration_ms);
  }
  f_lseek(&s->f, s->audio_start + off);
  s->in_len = s->in_pos = 0;
  s->eof = false;
  mp3dec_init(&s->dec);
  s->acc = s->src_rate;
  s->frames_out = (uint64_t)ms * s->out_rate / 1000U;
}

uint32_t mp3s_position_ms(const mp3s_t *s)
{
  return s->out_rate ? (uint32_t)(s->frames_out * 1000U / s->out_rate) : 0U;
}
