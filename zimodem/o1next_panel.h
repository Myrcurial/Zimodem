/*
   o1next_panel.h -- the O1NEXT WiFi COMM PAC's 11-lamp panel and P1 handshake.

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

   This is the *Arduino* half of the panel.  Everything that can be tested on a
   host is in `o1modem/panel.c` in the o1next repository -- the lamp map, the
   modem-state-to-lamp table, the RD/SD monostable and the register bytes.  What
   is here is the two things a host cannot have: `Wire`, and Zimodem's own state.

   The board is #324/#325, the work is #322, and the map is
   `hardware/wifi-comm-pac/pinmap.md`.
*/

#ifndef O1NEXT_PANEL_H
#define O1NEXT_PANEL_H

#include "panel.h"

/* Bring the two AW9523B parts up.  Returns false if either does not answer with
 * the right chip id (0x23), which is the one check that distinguishes "the panel
 * is dark" from "the panel is not there". */
bool o1nextPanelBegin(void);

/* Sample Zimodem's state, apply it to the panel, expire the pulses, and write
 * whatever changed.  Called from `loop()`; costs nothing when nothing changed. */
void o1nextPanelLoop(void);

/* The monostables.  Called from the two byte paths in `zstream.ino` -- one per
 * burst rather than one per byte, because the window is what stretches a byte
 * into something an eye can see. */
void o1nextPanelRdPulse(void);   /* data leaving for the machine */
void o1nextPanelSdPulse(void);   /* data arriving from the machine */

/* The "feel" rate (#320), in milli-baud.  #320 owns the real value; until it
 * lands this follows `baudRate`, so HS lights for a 9600 machine and stays dark
 * below it -- which is the same answer for every machine that is not in turbo. */
void o1nextPanelSetFeelRate(uint32_t mbaud);

/* The same number, read back.  The sound needs it too -- the connect score
 * follows the *feel* rate and not the ACIA rate (#320), and one source for that
 * fact is better than two. */
uint32_t o1nextPanelFeelRate(void);

/* Auto-answer, for the AA lamp.  The default reads Zimodem's own S60 listener
 * setting, which is the closest thing upstream has to an `ATAn` state. */
void o1nextPanelSetAutoAnswer(bool on);

/* A console/`AT` readout of the panel, one lamp per line.  #334's console is the
 * caller; it is here because "what does the panel think" is the first question
 * when the panel is wrong. */
void o1nextPanelDump(void);

#endif /* O1NEXT_PANEL_H */
