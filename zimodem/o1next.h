/*
   o1next.h -- the O1NEXT WiFi COMM PAC board profile.

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

   The board is an ESP32-S2 in an ESP32-S2-MINI-2U module.  The map, and the
   reason for every pin, is `hardware/wifi-comm-pac/pinmap.md` in the o1next
   repository (issues #324, #325, #330) -- **this file is its firmware half and
   the two must not drift.**

   Upstream has no S2 profile: the chain in zimodem.ino covers the C3 and the S3
   and then falls through to "standard ESP32 4 & 8MB boards", whose pins are not
   ours.  So this branch supplies the whole profile, and it is reached only when
   O1NEXT_BOARD is defined -- which zimodem.ino does for this fork.
*/

#ifndef O1NEXT_H
#define O1NEXT_H

/* ---------------------------------------------------------------- the UART
 *
 * UART1, on GPIO15/16 -- and deliberately *not* on UART1's IO-MUX default of
 * GPIO17/18.  Those two pins are also the only two DACs on the part (the IO MUX
 * entry is literally FUNC_DAC_1_U1TXD), and UART1 can be routed to any pin
 * through the GPIO matrix.  The movable function goes on movable pins.
 *
 * In role A these face the level stage and then the Osborne's MODEM port; in
 * role B they face the RP2354B, which serves the ACIA window.  The pins do not
 * change between roles -- only the counterparty does (#323).
 */
#define DEFAULT_PIN_TXD       GPIO_NUM_15
#define DEFAULT_PIN_RXD       GPIO_NUM_16

/* ------------------------------------------------------ the P1 handshake
 *
 * The DE-9 (P1) carries MSB, CTS, MCB and RI, and has **no DCD, no DSR and no
 * DTR**.  MSB, CTS and RI are driven from the AW9523B's I2C channels and MCB is
 * read from one, so none of the four is a GPIO; the two the connector does not
 * have are not fitted at all.  `-1` is how upstream disables a line.
 */
#define DEFAULT_PIN_DCD       -1
#define DEFAULT_PIN_CTS       -1
#define DEFAULT_PIN_RTS       -1
#define DEFAULT_PIN_RI        -1
#define DEFAULT_PIN_DSR       -1
#define DEFAULT_PIN_DTR       -1

/* Stream/command-mode flag: not wired on this board. */
#define DEFAULT_PIN_OTH       -1

/* The sound is the I2S amplifier -- BCLK/LRCLK/DIN on GPIO35/36/37 -- not a
 * beeper, so there is no single sound pin. */
#define DEFAULT_PIN_SND       -1

/* Left as upstream has them, and stated so the pin map and this file agree:
 *
 *   PIN_FACTORY_RESET   GPIO_NUM_0 -- the ROM bootloader's BOOT strap, and
 *                       upstream's factory reset when held while running.  One
 *                       button, two jobs.
 *   DEFAULT_BAUD_RATE   1200 -- upstream's default, and also an O1 rate, so the
 *                       out-of-box case works before any auto-baud exists (#319).
 *
 * Neither is redefined here: a redefinition would be identical, and a silent
 * no-op is worse documentation than a comment.
 */

/* ------------------------------------------------------- feature switches
 *
 * This board has no SD card, and two upstream features require one.  Undefining
 * them is how upstream's own C3 branch disables a feature, and it takes the SD
 * shell's own code with it (zbrowser.ino is guarded by INCLUDE_SD_SHELL).
 */
#undef INCLUDE_SD_SHELL
#undef INCLUDE_HOSTCM
#undef INCLUDE_COMET64

/* Commodore 1650/1660/1670 emulation and pulse dialling: not ours. */
#undef INCLUDE_CBMMODEM

/* INCLUDE_SSH stays on -- it is the point of the S2 (#321). */

/* --------------------------------------------------------------- the console
 *
 * The USB CDC is this board's console (see the FQBN: CDCOnBoot=cdc), and the
 * native-USB trap is that a boot banner printed before the host has opened the
 * port goes nowhere: the host is re-enumerating during the reset, and by the time
 * it is attached the firmware has already said everything it was going to say.
 * Measured, not assumed -- the first console read of a booted board returned
 * zero bytes.
 *
 * So the first debug line waits for the host, once, for at most
 * O1NEXT_USB_WAIT_MS.  The trade is deliberate: with a host attached the wait
 * ends as soon as it is seen, and with none attached it costs this much once, at
 * boot -- which is the case where nobody is reading the console anyway.
 */
#define O1NEXT_USB_WAIT_MS 2000u

static void o1next_usb_ready(void)
{
    static bool waited = false;
    unsigned     i;

    if (waited)
        return;
    waited = true;

    for (i = 0; i < (O1NEXT_USB_WAIT_MS / 10u); i++) {
        if (Serial)          /* the host has opened the port */
            break;
        delay(10);
    }
}

#endif /* O1NEXT_H */
