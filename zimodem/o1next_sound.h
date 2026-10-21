/*
   o1next_sound.h -- the O1NEXT WiFi COMM PAC's modem sound.

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

   This is the *Arduino* half of the sound.  The synthesis -- the oscillator, the
   four handshake scores, the scramblers -- is `o1modem/tone.c` in the o1next
   repository, and it is host-tested there.  What is here is the two things a host
   cannot have: the I2S peripheral, and Zimodem's own events.

   The board is #324/#325, the work is #322, and `docs/modem-tones.md` in the
   o1next repository is where every frequency comes from.
*/

#ifndef O1NEXT_SOUND_H
#define O1NEXT_SOUND_H

#include "tone.h"

/* Bring up I2S.  Returns false if the driver would not install, which on this
 * board means "no sound", not "no modem" -- so the caller keeps going. */
bool o1nextSoundBegin(void);

/* Feed the DMA buffer.  Called from `loop()`; a no-op when nothing is playing,
 * so the cost when idle is one comparison. */
void o1nextSoundLoop(void);

/* Play a score.  Replaces whatever is playing -- a second connect sound is a
 * second connect, not a queue. */
void o1nextSoundPlay(const o1_handshake *hs);

/* The handshake for a rate, in milli-baud.  `o1_hs_for_rate` does the choosing;
 * this is the convenience the named events below use. */
void o1nextSoundPlayForRate(uint32_t mbaud);

/* The named events.  Each is one line in the modem's own code, and each is
 * deliberately a *function* rather than a call to `o1nextSoundPlay`, so that the
 * policy -- which sound, and whether to make one at all -- lives in one place. */
void o1nextSoundConnect(uint32_t mbaud);  /* carrier up: the handshake */
void o1nextSoundRing(void);               /* a ring, for the ring cadence */
void o1nextSoundDial(void);               /* dial tone, while dialling */
void o1nextSoundDisconnect(void);
void o1nextSoundMute(bool on);
bool o1nextSoundIsMuted(void);
bool o1nextSoundIsPlaying(void);

/* What is playing, for the console.  NULL when silent. */
const char *o1nextSoundWhat(void);

#endif /* O1NEXT_SOUND_H */
