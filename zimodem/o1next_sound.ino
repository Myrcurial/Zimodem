/*
   o1next_sound.ino -- the O1NEXT WiFi COMM PAC's modem sound, on I2S.

   Copyright 2026 James Arlen

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   ---------------------------------------------------------------------------
   NEW FILE, added by the o1next project.  It is not part of upstream Zimodem.
   Apache-2.0 section 4(b) requires the change to be stated; this is it.
   ---------------------------------------------------------------------------

   The synthesis is `o1modem/tone.c` in the o1next repository -- an integer phase
   accumulator and four handshake scores, host-tested there and listenable with
   `render_wav.sh`.  This file is the I2S side and the policy.

   **The policy is deliberately one place.**  Which sound plays on a connect, and
   whether anything plays at all, is decided here and not at the call sites that
   know a modem event happened.  A panel whose sounds come from eleven places is a
   panel whose sounds disagree.

   The amp is a MAX98357A (U8) -- an I2S class-D amplifier with its own DAC, on
   the 5 V rail, driving a passive transducer.  BCLK/LRCLK/DIN are GPIO35/36/37
   from `hardware/wifi-comm-pac/pinmap.md`, and there is no gain or shutdown GPIO
   in the map: the amp's own straps set those, and the board's power switch is the
   mute that matters.
*/

#include <driver/i2s.h>

static o1_tone_gen o1nextTone;
static bool        o1nextSoundUp = false;
static bool        o1nextMuted   = false;
static bool        o1nextPlaying = false;
static int16_t     o1nextBuf[256];

/* The chunk is small on purpose.  `i2s_write` blocks until the DMA queue takes
 * the samples, so a 256-sample chunk is about 16 ms at 16 kHz -- short enough
 * that a handshake does not stall the modem's own loop noticeably, and long
 * enough that the DMA does not starve between calls. */
#define O1NEXT_SOUND_CHUNK 256u

/* A short wait rather than an indefinite one: if the DMA queue is full the right
 * answer is to come back next loop and drop this chunk's tail, not to block the
 * modem until the speaker catches up. */
#define O1NEXT_SOUND_WAIT_MS 20u

bool o1nextSoundBegin(void)
{
  i2s_config_t     cfg  = {};
  i2s_pin_config_t pins = {};

  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = O1_TONE_FS_DEFAULT;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB);
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;
  cfg.fixed_mclk           = 0;

  if(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK)
  {
    debugPrintf("O1NEXT sound: i2s_driver_install failed -- the panel will be silent.\r\n");
    return false;
  }

  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = (int)O1NEXT_I2S_BCLK;
  pins.ws_io_num    = (int)O1NEXT_I2S_LRCLK;
  pins.data_out_num = (int)O1NEXT_I2S_DIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  if(i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK)
  {
    debugPrintf("O1NEXT sound: i2s_set_pin failed -- the panel will be silent.\r\n");
    return false;
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  o1nextSoundUp = true;
  debugPrintf("O1NEXT sound: I2S up at %u Hz on %d/%d/%d.\r\n",
              (unsigned)O1_TONE_FS_DEFAULT,
              (int)O1NEXT_I2S_BCLK, (int)O1NEXT_I2S_LRCLK, (int)O1NEXT_I2S_DIN);
  return true;
}

void o1nextSoundPlay(const o1_handshake *hs)
{
  if((!o1nextSoundUp) || (hs == 0))
    return;

  if(o1nextMuted)
  {
    /* Muted means *silent*, not "queued for later" -- so a mute during a
     * handshake stops the handshake, which is what a mute switch does. */
    o1nextPlaying = false;
    return;
  }

  o1_tone_start(&o1nextTone, hs, O1_TONE_FS_DEFAULT, O1_TONE_AMP_DEFAULT);
  o1nextPlaying = true;
  debugPrintf("O1NEXT sound: %s, %u ms.\r\n", hs->name, (unsigned)o1_hs_ms(hs));
}

void o1nextSoundPlayForRate(uint32_t mbaud)
{
  o1nextSoundPlay(o1_hs_for_rate((o1_rate)mbaud));
}

void o1nextSoundConnect(uint32_t mbaud)
{
  o1nextSoundPlayForRate(mbaud);
}

void o1nextSoundRing(void)
{
  o1nextSoundPlay(&o1_hs_ring);
}

void o1nextSoundDial(void)
{
  o1nextSoundPlay(&o1_hs_dialtone);
}

void o1nextSoundDisconnect(void)
{
  o1nextSoundPlay(&o1_hs_disconnect);
}

void o1nextSoundMute(bool on)
{
  o1nextMuted = on;
  if(on && o1nextSoundUp)
  {
    o1nextPlaying = false;
    i2s_zero_dma_buffer(I2S_NUM_0);
  }
}

bool o1nextSoundIsMuted(void)
{
  return o1nextMuted;
}

bool o1nextSoundIsPlaying(void)
{
  return o1nextPlaying;
}

const char *o1nextSoundWhat(void)
{
  if((!o1nextPlaying) || (!o1nextSoundUp))
    return NULL;
  return o1_tone_phase_name(&o1nextTone);
}

void o1nextSoundLoop(void)
{
  unsigned n;
  size_t   written = 0;

  if((!o1nextPlaying) || (!o1nextSoundUp))
    return;

  n = o1_tone_render(&o1nextTone, o1nextBuf, O1NEXT_SOUND_CHUNK);
  if(n == 0)
  {
    o1nextPlaying = false;
    debugPrintf("O1NEXT sound: done.\r\n");
    return;
  }

  /* A short write is not an error -- it means the DMA queue was full, and the
   * next loop feeds it the rest.  Dropping the tail of a chunk is the correct
   * trade here: the alternative is blocking the modem on the speaker. */
  i2s_write(I2S_NUM_0, o1nextBuf, (size_t)n * sizeof(int16_t), &written,
            O1NEXT_SOUND_WAIT_MS / portTICK_PERIOD_MS);

  if(o1_tone_done(&o1nextTone))
  {
    o1nextPlaying = false;
    debugPrintf("O1NEXT sound: done.\r\n");
  }
}
