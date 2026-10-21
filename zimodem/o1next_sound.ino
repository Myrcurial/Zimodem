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
static bool        o1nextPlaying = false;
static int16_t     o1nextBuf[256];

/* -------------------------------------------------------- the speaker, and the
 * dial that holds a connection back.
 *
 * The table -- Hayes' four speaker modes, and how they compose with the `T` in
 * `ATDT` -- is `o1_sound_policy` in tone.h, where it is host-tested.  This is the
 * fork's single instance of it.  **There is one policy and one mute**: `Mute` is
 * the boolean view of `ATM0`, not a second mechanism, because two mutes is
 * exactly the drift this file's notice warns about.
 */
static o1_sound_policy o1nextPolicy;
static bool           o1nextPolicyReady = false;

/* The dial, and the gate.  `o1_dial_gate` holds the two facts a connection needs
 * before it is *admitted* -- the tones have finished, and the link is up -- and
 * they arrive from different places, which is why they are held rather than
 * inferred.  Either order works: a fast host is waited for, a slow dial is waited
 * out. */
static o1_dial_gate o1nextDialGate;
static o1_dtmf_dial o1nextDial;
static bool         o1nextDialPlaying = false;
static char         o1nextDialDigits[O1_DTMF_DIGITS_MAX + 1u];

static void o1nextPolicyInit(void)
{
  if(o1nextPolicyReady)
    return;
  o1nextPolicyReady = true;
  o1_sound_policy_init(&o1nextPolicy);   /* M1, and a call that has not asked */
  o1_dial_gate_reset(&o1nextDialGate);
}

/* "The sound has stopped" -- and the one place that knows a *dial* has finished,
 * which is a fact the admission gate is waiting for.
 *
 * **Every way a dial can end comes through here**, a mute in the middle of one
 * included.  That is deliberate: a dial cut short by `ATM0` has still finished,
 * and a gate left waiting for tones that will never resume would hold the
 * connection open for the rest of the session. */
static void o1nextSoundStopped(void)
{
  o1nextPlaying = false;
  if(o1nextDialPlaying)
  {
    o1nextDialPlaying = false;
    o1_dial_gate_dialled(&o1nextDialGate);
  }
}

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

  /* Before the I2S attempt, not after: the speaker mode has to work -- and has to
   * be reportable by `ATM` -- even on a board whose amplifier would not install. */
  o1nextPolicyInit();

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
  o1nextPolicyInit();

  if((!o1nextSoundUp) || (hs == 0))
    return;

  /* A new sound replaces the old one -- a dial included, which is therefore
   * marked finished here too.  This is the primitive; whether a sound should be
   * made at all is the *policy*, and the policy is asked by the named events
   * below rather than here, so that there is one answer per event and not two. */
  o1nextSoundStopped();

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
  o1nextPolicyInit();

  /* M3 is the mode that stops at the dial, and a plain `ATD` never asked. */
  if(!o1_sound_wants_connect(&o1nextPolicy))
    return;

  o1nextSoundPlayForRate(mbaud);
}

void o1nextSoundRing(void)
{
  o1nextPolicyInit();
  if(!o1_sound_wants_ring(&o1nextPolicy))
    return;
  o1nextSoundPlay(&o1_hs_ring);
}

void o1nextSoundDial(void)
{
  /* The exchange's dial tone, on its own.  `ATDT` does **not** use this: the dial
   * a call makes is its own DTMF fingerprint, and prepending a second of dial
   * tone to it would be inventing a phase nobody asked for. */
  o1nextPolicyInit();
  if(!o1_sound_wants_dial(&o1nextPolicy))
    return;
  o1nextSoundPlay(&o1_hs_dialtone);
}

void o1nextSoundDisconnect(void)
{
  /* Not a dial, and there is no `T` for it, so the mode alone decides -- and the
   * mode that says yes is M2, the only one still on after the carrier.  This is
   * where M1 and M2 part. */
  o1nextPolicyInit();
  if(!o1_sound_wants_disconnect(&o1nextPolicy))
    return;
  o1nextSoundPlay(&o1_hs_disconnect);
}

/* ---------------------------------------------------------------- the policy */

void o1nextSoundSetMode(o1_speaker m)
{
  o1nextPolicyInit();
  o1_sound_policy_set_mode(&o1nextPolicy, m);

  /* A mute is immediate: silence, not a queue.  A mute switch that lets the
   * handshake finish first is not a mute switch. */
  if(m == O1_SPK_OFF)
  {
    o1nextSoundStopped();
    if(o1nextSoundUp)
      i2s_zero_dma_buffer(I2S_NUM_0);
  }

  debugPrintf("O1NEXT sound: speaker %s.\r\n", o1_speaker_name(m));
}

o1_speaker o1nextSoundMode(void)
{
  o1nextPolicyInit();
  return o1_sound_policy_mode(&o1nextPolicy);
}

const char *o1nextSoundModeName(void)
{
  return o1_speaker_name(o1nextSoundMode());
}

void o1nextSoundMute(bool on)
{
  /* The boolean view of `ATM0` and `ATM1` -- one policy, two ways to say it. */
  o1nextSoundSetMode(on ? O1_SPK_OFF : O1_SPK_UNTIL_CARRIER);
}

bool o1nextSoundIsMuted(void)
{
  return o1nextSoundMode() == O1_SPK_OFF;
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
    o1nextSoundStopped();
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
    o1nextSoundStopped();
    debugPrintf("O1NEXT sound: done.\r\n");
  }
}

/* ------------------------------------------------------- the dial, and the gate
 *
 * `ATDT <host>` plays the DTMF fingerprint of the connect string, and the
 * connection is not *admitted* until it has finished -- option (c) of #322.  The
 * socket, the stream and the machine's own CONNECT are immediate; what waits is
 * the CD lamp and the `CONNECT` text, because those two are what "the connection
 * exists" means to everything watching.
 *
 * The call order is the shape of the thing:
 *
 *     o1nextDialBegin(connect, tones)  before the connect attempt, so the tones
 *                                      play while the socket comes up
 *     o1nextDialLinked()               the socket answered
 *     o1nextDialFailed()               it did not, so there is nothing to wait for
 *     o1nextDialAdmitted()             both facts in -- `loop()` releases it
 */

void o1nextDialBegin(const char *connect, bool tones)
{
  unsigned n;

  o1nextPolicyInit();

  /* This call's request, from the `T` in `ATDT`.  Set on *every* dial, including
   * the silent ones, so that a previous call's `T` cannot leak into an `ATD`:
   * a sticky setting would make the sound stop meaning what was typed. */
  o1_sound_policy_set_tones(&o1nextPolicy, tones ? 1 : 0);
  o1_dial_gate_reset(&o1nextDialGate);
  o1nextDialPlaying = false;   /* a previous dial's tail must not mark this one */
  o1nextDialDigits[0] = '\0';

  if(!o1_sound_wants_dial(&o1nextPolicy))
  {
    /* A silent dial is still a dial: the socket opens and the connection is
     * admitted the moment the link is up, because there are no tones to wait for.
     * The `o1_dial_gate_reset` above is what makes that true -- an unarmed gate
     * admits everything, which is what keeps an ordinary connection ordinary. */
    debugPrintf("O1NEXT dial: silent (%s%s).\r\n", o1_speaker_name(o1nextSoundMode()),
                tones ? ", +T" : "");
    return;
  }

  if(!o1nextSoundUp)
  {
    debugPrintf("O1NEXT dial: no I2S, so there is nothing to wait for.\r\n");
    return;
  }

  n = o1_dtmf_dial_build(&o1nextDial, connect, O1_DTMF_DIGITS_MAX);
  if(n == 0u)
  {
    debugPrintf("O1NEXT dial: \"%s\" has no fingerprint.\r\n",
                (connect != 0) ? connect : "(null)");
    return;
  }

  strncpy(o1nextDialDigits, o1nextDial.digits, sizeof o1nextDialDigits - 1u);
  o1nextDialDigits[sizeof o1nextDialDigits - 1u] = '\0';

  /* Armed *before* playing, never after: a short score can finish inside this
   * call on a slow enough loop, and arming afterwards would lose the fact. */
  o1_dial_gate_arm(&o1nextDialGate);
  o1nextSoundPlay(&o1nextDial.hs);
  o1nextDialPlaying = o1nextPlaying;

  debugPrintf("O1NEXT dial: %s, %u ms -- \"%s\".\r\n", o1nextDialDigits,
              (unsigned)o1_hs_ms(&o1nextDial.hs),
              (connect != 0) ? connect : "(null)");
}

void o1nextDialLinked(void)
{
  o1nextPolicyInit();
  o1_dial_gate_linked(&o1nextDialGate);
}

void o1nextDialFailed(void)
{
  o1nextPolicyInit();
  /* Not "the link dropped" -- that is a *disconnect*, and it happens after
   * admission.  This is "the attempt failed", so there is nothing left to wait
   * for and the next dial starts clean. */
  o1_dial_gate_abort(&o1nextDialGate);
  o1nextDialPlaying = false;
  o1nextDialDigits[0] = '\0';
}

bool o1nextDialAdmitted(void)
{
  o1nextPolicyInit();
  /* Armed *and* both facts.  An unarmed gate is admitted by definition -- there
   * is no dial to wait for -- so a caller that needs to know whether an admission
   * is a *deferred* one must ask `o1nextDialHolding()` first. */
  return o1_dial_gate_armed(&o1nextDialGate) && o1_dial_gate_admitted(&o1nextDialGate);
}

bool o1nextDialHolding(void)
{
  o1nextPolicyInit();
  /* A dial is running and has not been admitted: something is waiting. */
  return o1_dial_gate_armed(&o1nextDialGate) && !o1_dial_gate_admitted(&o1nextDialGate);
}

void o1nextDialSpent(void)
{
  o1nextPolicyInit();
  o1_dial_gate_reset(&o1nextDialGate);
}

const char *o1nextDialled(void)
{
  return (o1nextDialDigits[0] != '\0') ? o1nextDialDigits : NULL;
}
