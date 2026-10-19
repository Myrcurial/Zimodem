/*
   o1next_console.ino -- a console you can type at, on the board's USB port.

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
   NEW FILE, added by the o1next project (#334, part of #56).  It is not part of
   upstream Zimodem.  Apache-2.0 section 4(b) requires the change to be stated;
   this is it.
   ---------------------------------------------------------------------------

   WHY THIS EXISTS

   The board's console is the USB CDC (`CDCOnBoot=cdc`, see o1next.h), and until
   now it was WRITE-ONLY: debugPrintf() mirrored the firmware's log out of it and
   nothing ever read it back.  So there was no way to type at a running board and
   no way to ask it anything -- not what it had loaded, not which slot it booted
   from, not whether it was the firmware we thought we flashed.  This is the read
   half, and the four commands the first-light session asked for (#334).

   IT IS NOT THE MODEM'S SERIAL PORT.  The O1 side is UART1 on GPIO15/16 and the
   ROM bootloader's own log is UART0; this console is a third, independent stream
   on the native USB.  Nothing here reads or writes HWSerial, so a command typed
   here cannot be mistaken for a byte the Osborne sent, and no byte the Osborne
   sent can be eaten by the console.

   WHY `bootloader` IS THE ONE THAT MATTERS

   The only programming port on this board is the ESP32-S2's own USB, and the ROM
   cannot reset the chip over it -- which is a button press, on a device whose USB
   port is documented as being for programming.  esptool's own reset sequence
   papers over that on a board WITH an auto-reset circuit, which is the board we
   brought up on.  **Our own PCB will not have one**, so there the button is the
   only way into the ROM -- unless the running firmware can do it.  It can, and
   the mechanism is the RTC domain's force-download-boot bit rather than the BOOT
   strap: the strap is the obvious answer and it was measured not to work, so the
   comment on the command carries that measurement rather than a repetition of it
   here.
*/

#include "o1next_console.h"

#if defined(O1NEXT_BOARD) && ARDUINO_USB_CDC_ON_BOOT

#include <ctype.h>
#include <string.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>

/* The line buffer.  A command is a word today and a word plus a length later
 * (the OTA path), so this is generous rather than tuned -- and a line that
 * overflows is dropped and SAID to be dropped, rather than silently truncated
 * into a different command. */
#define O1NEXT_CONSOLE_LINE_MAX 64

static char   o1nextConsoleLine[O1NEXT_CONSOLE_LINE_MAX];
static size_t o1nextConsoleLen   = 0;
static bool   o1nextConsoleFull  = false; /* this line overflowed; drop it */
static bool   o1nextConsoleAtEOL = false; /* a terminator was the last thing seen */

/* Plain literals, no F(): on the ESP32 a string literal is already in flash and
 * F() is a cast, so it buys nothing -- and Print::printf() takes a const char*,
 * which F() is not.  Upstream's own strings are plain for the same reason. */
static void o1nextConsolePrompt(void)
{
    Serial.print("o1next> ");
}

/* Announce the console, once, and only to a host that is actually listening.
 * The USB CDC is not the UART0 the ROM logs to: a line printed before the host
 * opens the port goes nowhere, which is the whole reason o1next_usb_ready()
 * exists (see o1next.h).  So this waits to be seen rather than waiting to print,
 * and it is the console that yields, not the boot banner. */
static void o1nextConsoleGreet(void)
{
    static bool greeted = false;

    if (greeted || !Serial)
        return;
    greeted = true;
    Serial.printf("\r\no1next console -- Zimodem %s.  Type help for the commands.\r\n",
                  ZIMODEM_VERSION);
    o1nextConsolePrompt();
}

/* ------------------------------------------------------------------- status --
 *
 * The four facts the first-light session wanted from a running board, plus the
 * one that explains the board's most confusing behaviour: what GPIO0 -- the ROM's
 * BOOT strap, and the factory-reset input -- reads right now.  A board whose
 * auto-reset circuit holds it low cannot be told from one that has been asked to
 * enter download mode, and that is exactly the confusion the flash script had to
 * document.
 *
 * `uptime` is millis(), and what zeroes it is worth knowing before it is trusted.
 * esp_restart() does -- measured 6703.5 s, `reboot`, 9.6 s -- and so does
 * esptool's own reset on a running application: after a flash that began with one,
 * 20.0 s.  **The reset esptool performs at the end of a stub session does not**:
 * after one, a freshly started application reported 6664.9 s, and after a flash
 * driven from that stub, 346.5 s, counting from the last esp_restart.  So it is a
 * real number, and it is not always "how long has this application been up" --
 * which is what the `built` line above is for.
 */
static void o1nextConsoleStatus(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(NULL);
    uint8_t      mac[6];
    unsigned long up = millis();

    Serial.printf("\r\nZimodem %s  (built %s)\r\n", ZIMODEM_VERSION, compile_date);

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
        Serial.printf("board    ESP32-S2, flash %u bytes, MAC %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                      (unsigned)ESP.getFlashChipSize(),
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    else
        Serial.printf("board    ESP32-S2, flash %u bytes, MAC unreadable\r\n",
                      (unsigned)ESP.getFlashChipSize());

    /* The pins in use, and the ones that are not: -1 means "not fitted", which is
     * how upstream disables a line, and most of ours are -1 because the panel and
     * the handshake hang off the AW9523B's I2C channels rather than off GPIO. */
    Serial.printf("serial   O1 side is UART1: RXD %d, TXD %d, %d bps\r\n",
                  (int)DEFAULT_PIN_RXD, (int)DEFAULT_PIN_TXD, baudRate);
    Serial.printf("         handshake DCD %d, CTS %d, RTS %d, RI %d, DSR %d, DTR %d\r\n",
                  (int)DEFAULT_PIN_DCD, (int)DEFAULT_PIN_CTS, (int)DEFAULT_PIN_RTS,
                  (int)DEFAULT_PIN_RI, (int)DEFAULT_PIN_DSR, (int)DEFAULT_PIN_DTR);
    Serial.printf("         console is this USB CDC; ROM log is UART0\r\n");
    Serial.printf("strap    GPIO%d (BOOT / factory reset) reads %s\r\n",
                  (int)PIN_FACTORY_RESET,
                  digitalRead(PIN_FACTORY_RESET) ? "HIGH" : "LOW");

    /* Which slot the running image came out of, and where the next one goes --
     * the second slot is the whole reason for min_spiffs, and until the OTA path
     * lands (#334, item 2) this is the only way to see it is really there. */
    if (running != NULL)
        Serial.printf("slot     running %s at 0x%06x, %u bytes\r\n",
                      running->label, (unsigned)running->address, (unsigned)running->size);
    else
        Serial.print("slot     unknown -- esp_ota_get_running_partition() failed\r\n");

    if (next != NULL)
        Serial.printf("         next update would go to %s at 0x%06x\r\n",
                      next->label, (unsigned)next->address);
    else
        Serial.print("         no second application slot -- OTA is not possible\r\n");

    Serial.printf("uptime   %lu.%01lu s\r\n", up / 1000UL, (up % 1000UL) / 100UL);
    Serial.printf("heap     free %u, min free since boot %u\r\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
}

/* -------------------------------------------------------- reboot, bootloader --
 *
 * esp_restart() is the same system reset the ROM performs on a power cycle, so
 * both of these re-sample the BOOT strap on the way up.  That is the entire
 * difference between them: `reboot` leaves GPIO0 alone and comes back into the
 * application, `bootloader` drives it low first and comes back into the ROM's
 * download mode.  The 50 ms is for the pin to settle before the strap is read.
 *
 * The flush before the reset is not decoration: the CDC's transmit is not
 * instant, and a reset that arrives first takes the sentence explaining itself
 * with it -- which is how a console command comes to look like it did nothing.
 */
static void o1nextConsoleReboot(void)
{
    Serial.print("rebooting into the application\r\n");
    Serial.flush();
    delay(50);
    esp_restart();
}

static void o1nextConsoleBootloader(void)
{
    Serial.print("forcing the ROM's download mode, then resetting -- no button needed\r\n");
    Serial.flush();

    /* The strap route, and it is NOT the half that works here.
     *
     * The obvious implementation is the BOOT strap: drive GPIO0 low, reset, and
     * let the ROM sample it.  **Measured on the bring-up board 2026-10-19, and it
     * does not work**: the port drops and the application comes back, banner and
     * all.  An SW system reset does not re-sample the BOOT strap -- the mode
     * latched by the last *hardware* reset persists -- which also explains the
     * older observation in tools/flash-o1next.sh, where esptool's write_mem of
     * SW_SYS_RST landed, dropped the port, and came back in download mode: the
     * chip was already in download mode, so the latched mode was download mode.
     *
     * What does work is the RTC domain's own force-download-boot bit, which is
     * the mechanism the Arduino core itself uses to restart into USB download
     * mode (cores/esp32/esp32-hal-tinyusb.c, usb_persist_restart).  It lives in
     * the RTC domain, so it survives the SW reset, and the ROM honours it on the
     * way back up.
     *
     * GPIO0 is still driven low, and it is the half that matters if the reset
     * that follows is a hardware one -- which is the case on a board whose EN pin
     * something else can pulse, ours included.  It costs two lines and covers the
     * reset this one does not. */
    pinMode(PIN_FACTORY_RESET, OUTPUT);
    digitalWrite(PIN_FACTORY_RESET, LOW);

    SET_PERI_REG_MASK(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);

    delay(50);
    esp_restart();
}

/* ------------------------------------------------------------------- update --
 *
 * The hook, and deliberately nothing more yet.  The second slot is real and
 * empty, the artifact is the application .bin rather than the merged image, and
 * the path is: write the incoming image to the INACTIVE slot, then reboot into
 * it -- which needs no cable, no button and no download mode (#334, item 2).
 *
 * Saying so out loud is the point of the command existing now: `update` is a
 * reserved word with a known answer, not a word that might mean something.
 */
static void o1nextConsoleUpdate(void)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    Serial.print("\r\nupdate: no image being received -- this is the hook, not the path yet.\r\n");
    if (next != NULL)
        Serial.printf("        an image would go to %s at 0x%06x (%u bytes),\r\n"
                      "        then the board would reboot into it.\r\n",
                      next->label, (unsigned)next->address, (unsigned)next->size);
    else
        Serial.print("        there is no inactive slot to write to.\r\n");
    Serial.print("        the artifact is zimodem.ino.bin, the application alone --\r\n"
                 "        not the merged image.  Until this lands, a new image goes\r\n"
                 "        in over USB with tools/flash-o1next.sh.\r\n");
}

/* --------------------------------------------------------------------- help --
 *
 * A console with no help is a guessing game, and the bootloader case is the one
 * worth a sentence: it is the reason this file exists before the OTA path does.
 */
static void o1nextConsoleHelp(void)
{
    Serial.print(
        "\r\ncommands:\r\n"
        "  status      version, pins in use, the running slot, uptime, free heap\r\n"
        "  reboot      esp_restart() -- back into the application\r\n"
        "  bootloader  force the ROM's download mode and reset -- no button.  On a\r\n"
        "              board with no auto-reset circuit -- ours -- this is the only\r\n"
        "              way back to a flash tool.\r\n"
        "  update      the OTA hook; nothing is received yet (#334, item 2)\r\n"
        "  help        this\r\n"
        "\r\n"
        "  The console is the USB CDC.  The O1 side is UART1 (GPIO15/16) and is\r\n"
        "  not this stream: nothing typed here reaches the machine, and nothing\r\n"
        "  the machine sends is read here.\r\n");
}

/* ---------------------------------------------------------------- dispatch --
 *
 * Case is folded rather than demanded, because a console typed at through a
 * serial monitor should not fail on shift -- and upstream's own command mode is
 * case-insensitive for the same reason.  An unknown word says so and names the
 * one command that lists the others; it does not guess at a near-miss.
 */
static void o1nextConsoleRun(void)
{
    char *cmd = o1nextConsoleLine;

    while (*cmd == ' ' || *cmd == '\t')
        cmd++;

    for (char *p = cmd; *p != '\0'; p++)
        *p = (char)tolower((unsigned char)*p);

    if (*cmd == '\0')
        return;
    else if (!strcmp(cmd, "status"))
        o1nextConsoleStatus();
    else if (!strcmp(cmd, "reboot"))
        o1nextConsoleReboot();
    else if (!strcmp(cmd, "bootloader"))
        o1nextConsoleBootloader();
    else if (!strcmp(cmd, "update"))
        o1nextConsoleUpdate();
    else if (!strcmp(cmd, "help") || !strcmp(cmd, "?"))
        o1nextConsoleHelp();
    else
        Serial.printf("unknown command: %s -- try help\r\n", cmd);
}



/* ------------------------------------------------------------------ the loop --
 *
 * One line in, one line out.  The echo is deliberate: the terminal that types at
 * a device console does not always echo what it sends, and a console you cannot
 * see yourself type at is a console you cannot trust.  A terminal that echoes
 * locally as well will show it twice -- that is the terminal, not this.
 *
 * Nothing here waits.  A partially typed line lives in a static buffer between
 * calls, and loop() is where the modem's own work happens, so a console that
 * blocked on a keystroke would stop the modem.
 */
void o1nextConsolePoll(void)
{
    int c;

    o1nextConsoleGreet();

    while ((c = Serial.read()) >= 0)
    {
        if (c == '\r' || c == '\n')
        {
            /* A terminal sends CRLF; that is one line, not a line and an empty
             * one, so the terminator that follows another is not a new line. */
            if (o1nextConsoleAtEOL)
                continue;
            o1nextConsoleAtEOL = true;

            Serial.print("\r\n");
            if (o1nextConsoleFull)
            {
                Serial.printf("line too long (max %d) -- dropped\r\n",
                              O1NEXT_CONSOLE_LINE_MAX - 1);
            }
            else
            {
                o1nextConsoleLine[o1nextConsoleLen] = '\0';
                o1nextConsoleRun();
            }
            o1nextConsoleLen  = 0;
            o1nextConsoleFull = false;
            o1nextConsolePrompt();
            continue;
        }

        o1nextConsoleAtEOL = false;

        if (c == 0x7f || c == 0x08)          /* delete / backspace */
        {
            if (o1nextConsoleFull)
                continue;                    /* the line is already dropped */
            if (o1nextConsoleLen > 0)
            {
                o1nextConsoleLen--;
                Serial.print("\b \b");
            }
            continue;
        }

        if (c < 0x20 || c > 0x7e)            /* other control bytes, and UTF-8 */
            continue;

        if (o1nextConsoleLen >= O1NEXT_CONSOLE_LINE_MAX - 1)
        {
            o1nextConsoleFull = true;        /* say so at the end of the line */
            continue;
        }

        o1nextConsoleLine[o1nextConsoleLen++] = (char)c;
        Serial.write((uint8_t)c);            /* echo */
    }
}

#endif /* O1NEXT_BOARD && ARDUINO_USB_CDC_ON_BOOT */

