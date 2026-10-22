/*
   o1next_panel.ino -- the 11-lamp panel and the P1 handshake, on two AW9523B.

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

   Everything testable lives in `o1modem/panel.c` in the o1next repository: the
   lamp-to-channel map, the modem-state-to-lamp table, the RD/SD monostable and
   the register bytes.  This file is the two things a host cannot have -- `Wire`,
   and Zimodem's own state -- and it is deliberately thin for that reason.

   The panel is written **on change only**.  That is the property #322 asks for
   and it is worth restating: the AW9523B holds its outputs, so a hung firmware
   leaves a readable frozen panel instead of a dark one -- and a panel rewritten
   every loop is a panel that goes blank in the failure it was meant to report.
*/

#include <Wire.h>

static o1_panel       o1nextPanel;
static o1_modem_state o1nextState;
static o1_panel_plan  o1nextPlan;
static bool           o1nextPanelUp = false;

/* The interface expander is a DNP option, so whether the handshake lines exist is
 * a runtime fact rather than a compile-time one. */
static bool o1nextIfaceUp = false;

/* The "feel" rate (#320), in milli-baud.  #320 owns the real value; until it
 * lands this follows the ACIA's rate, which gives the right answer for every
 * machine that is not in turbo (#55). */
static uint32_t o1nextFeelMbaud = 0;

/* Auto answer, for the AA lamp.  Upstream has no `ATAn` state to read -- the
 * closest thing is S60, "preserve listeners" -- so that is the default and the
 * setter exists for whoever wires the real one. */
static int o1nextAutoAnswer = -1;   /* -1 = follow S60 */

static bool o1nextI2cWrite(uint8_t addr, uint8_t reg, uint8_t val)
{
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static int o1nextI2cRead(uint8_t addr, uint8_t reg)
{
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if(Wire.endTransmission(false) != 0)
    return -1;
  if(Wire.requestFrom((uint16_t)addr, (uint8_t)1) != 1)
    return -1;
  return (int)Wire.read();
}

static bool o1nextRunSeq(uint8_t addr, const o1_reg_write *seq, unsigned n)
{
  unsigned i;

  for(i = 0; i < n; i++)
    if(!o1nextI2cWrite(addr, seq[i].reg, seq[i].val))
      return false;
  return true;
}

bool o1nextPanelBegin(void)
{
  o1_reg_write seq[32];
  unsigned     n;
  int          id;

  Wire.begin((int)O1NEXT_I2C_SDA, (int)O1NEXT_I2C_SCL);

  /* The chip id is the one check that separates "the panel is dark" from "the
   * panel is not there", and it is a read rather than a guess. */
  id = o1nextI2cRead(O1_AW9523_ADDR_PANEL, O1_AW9523_REG_CHIPID);
  if(id != (int)O1_AW9523_CHIPID_VALUE)
  {
    debugPrintf("O1NEXT panel: %02X answered %d, wanted %u -- panel left dark.\r\n",
                (unsigned)O1_AW9523_ADDR_PANEL, id, (unsigned)O1_AW9523_CHIPID_VALUE);
    return false;
  }

  n = o1_panel_init_writes(seq, (unsigned)(sizeof seq / sizeof seq[0]));
  if((n == 0) || (!o1nextRunSeq(O1_AW9523_ADDR_PANEL, seq, n)))
  {
    debugPrintf("O1NEXT panel: the panel expander refused its init sequence.\r\n");
    return false;
  }

  /* The interface expander is a DNP option (#325).  Without it the panel still
   * works and the handshake lines are simply not driven -- so this is a notice
   * and not a failure. */
  id = o1nextI2cRead(O1_AW9523_ADDR_IFACE, O1_AW9523_REG_CHIPID);
  if(id == (int)O1_AW9523_CHIPID_VALUE)
  {
    n = o1_iface_init_writes(seq, (unsigned)(sizeof seq / sizeof seq[0]));
    if((n == 0) || (!o1nextRunSeq(O1_AW9523_ADDR_IFACE, seq, n)))
    {
      debugPrintf("O1NEXT panel: the interface expander refused its init sequence.\r\n");
      return false;
    }
    o1nextIfaceUp = true;
  }
  else
    debugPrintf("O1NEXT panel: no interface expander at %02X (answered %d) -- "
                "handshake lines not driven.\r\n",
                (unsigned)O1_AW9523_ADDR_IFACE, id);

  o1_panel_init(&o1nextPanel);
  memset(&o1nextState, 0, sizeof o1nextState);

  if(o1nextFeelMbaud == 0)
    o1nextPanelSetFeelRate((uint32_t)baudRate * 1000u);

  o1nextPanelUp = true;
  debugPrintf("O1NEXT panel: up -- %u lamps, %u dim registers.\r\n",
              (unsigned)O1_LAMP_COUNT, 16u);
  return true;
}


/* MCB is P1 pin 8, "low suppresses output" -- the machine's DTR-equivalent, and
 * the source of the TR lamp (#322).  It is the one *input* on the interface part,
 * which is why that part needs an INTN line at all. */
static int o1nextMcbReady(void)
{
  int v;

  if(!o1nextIfaceUp)
    return 1;   /* nothing on the line to suppress us, and nothing to read */

  v = o1nextI2cRead(O1_AW9523_ADDR_IFACE, O1_AW9523_REG_INPUT0 + 1u);
  if(v < 0)
    return 1;

  /* The polarity is the machine's, so "ready" is the inverse of the pin. */
  return ((v >> O1_IF_MCB_BIT) & 1) ? 0 : 1;
}

static void o1nextSampleState(void)
{
  int open = WiFiClientNode::getNumOpenWiFiConnections();

  o1nextState.wifi_started   = (WiFi.getMode() & WIFI_STA) ? 1 : 0;
  o1nextState.wifi_connected = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
  o1nextState.feel           = o1nextFeelMbaud;

  o1nextState.auto_answer    = (o1nextAutoAnswer >= 0)
                                 ? o1nextAutoAnswer
                                 : (commandMode.o1nextPreserveListeners() ? 1 : 0);

  /* **There is no DCD on P1** (pinmap.md, "Open before layout" 3), so the CD
   * lamp is socket state -- which is what `dcdStatus` already tracks: it goes
   * active when a connection opens and inactive when the last one closes. */
  o1nextState.carrier        = (dcdStatus == dcdActive) ? 1 : 0;
  o1nextState.off_hook       = (open > 0) ? 1 : 0;
  o1nextState.terminal_ready = o1nextMcbReady();
  o1nextState.modem_ready    = o1nextPanelUp ? 1 : 0;
  o1nextState.ring           = (commandMode.o1nextRingCounter() > 0) ? 1 : 0;
  o1nextState.cts            = 1;
  o1nextState.msb            = (dcdStatus == dcdActive) ? 1 : 0;
}

void o1nextPanelLoop(void)
{
  unsigned i, total;

  if(!o1nextPanelUp)
    return;

  o1nextSampleState();
  o1_panel_apply(&o1nextPanel, &o1nextState, millis());
  o1_panel_tick(&o1nextPanel, millis());

  total = o1_panel_flush(&o1nextPanel, &o1nextState, &o1nextPlan);
  if(total == 0)
    return;   /* nothing changed -- and that "nothing" is what freezes the panel */

  for(i = 0; i < o1nextPlan.n_dim; i++)
  {
    if(!o1nextI2cWrite(O1_AW9523_ADDR_PANEL, o1nextPlan.dim[i].reg, o1nextPlan.dim[i].val))
    {
      /* A failed write means the driver and the part disagree about what is on
       * the wire, so the whole panel is rewritten next time rather than only what
       * changed.  (See `o1_panel_invalidate`.) */
      debugPrintf("O1NEXT panel: write %02X=%02X failed -- panel invalidated.\r\n",
                  (unsigned)o1nextPlan.dim[i].reg, (unsigned)o1nextPlan.dim[i].val);
      o1_panel_invalidate(&o1nextPanel);
      return;
    }
  }

  /* With no interface expander the handshake writes are dropped rather than
   * retried forever -- there is nothing at that address and nothing to say. */
  if(o1nextIfaceUp)
  {
    for(i = 0; i < o1nextPlan.n_iface; i++)
    {
      if(!o1nextI2cWrite(O1_AW9523_ADDR_IFACE, o1nextPlan.iface[i].reg, o1nextPlan.iface[i].val))
      {
        o1_panel_invalidate(&o1nextPanel);
        return;
      }
    }
  }
}

void o1nextPanelRdPulse(void)
{
  if(o1nextPanelUp)
    o1_panel_pulse(&o1nextPanel, O1_LAMP_RD, millis());
}

void o1nextPanelSdPulse(void)
{
  if(o1nextPanelUp)
    o1_panel_pulse(&o1nextPanel, O1_LAMP_SD, millis());
}

void o1nextPanelSetFeelRate(uint32_t mbaud)
{
  o1nextFeelMbaud = mbaud;
}

uint32_t o1nextPanelFeelRate(void)
{
  return o1nextFeelMbaud;
}

void o1nextPanelSetAutoAnswer(bool on)
{
  o1nextAutoAnswer = on ? 1 : 0;
}

void o1nextPanelDump(void)
{
  unsigned i;

  debugPrintf("O1NEXT panel: %s, feel %u mbaud, MCB %s\r\n",
              o1nextPanelUp ? "up" : "DOWN",
              (unsigned)o1nextFeelMbaud,
              o1nextMcbReady() ? "ready" : "suppressed");

  for(i = 0; i < (unsigned)O1_LAMP_COUNT; i++)
    debugPrintf("  %-11s ch %2d  %s\r\n",
                o1_lamp_name((o1_lamp)i),
                o1_lamp_channel((o1_lamp)i),
                o1_panel_get(&o1nextPanel, (o1_lamp)i) ? "on" : "off");
}
