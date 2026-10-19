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

   AND `update` IS THE OTHER ONE

   A board with no auto-reset circuit and no button can only be re-flashed if the
   running firmware can reach the ROM -- and the better answer is not to need the
   ROM at all.  The second slot has existed since the partition scheme changed to
   `min_spiffs`, and nothing used it.  `update` uses it: the application image
   arrives on this console, goes into the INACTIVE slot, is verified there, and
   the board reboots into it.  No cable moved, no button, no download mode.
   `bootloader` is what is still there if the new image does not come up.

   AND THE NETWORK IS WHAT MAKES IT REMOTE

   `update <url>` fetches that same image over HTTP, and `update-at-boot <url>`
   arms one fetch for the next boot -- so an update can happen with nothing plugged
   into the board at all.  `wifi` joins a network and saves it, through the same
   config file the modem's own AT commands write: a board on a bench has no machine
   on the O1 side to configure it with, and two places that can hold an SSID is how
   a board ends up on the wrong network.
*/

#include "o1next_console.h"

#if defined(O1NEXT_BOARD) && ARDUINO_USB_CDC_ON_BOOT

#include <ctype.h>
#include <string.h>
#include <Update.h>          /* the OTA path below: begin/write/end, and the slot */
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
static size_t o1nextConsoleLen      = 0;
static bool   o1nextConsoleFull     = false; /* this line overflowed; drop it */
static char   o1nextConsoleLastTerm = 0;     /* the last terminator seen, else 0 */

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
 * The second slot, used.  The path is the one #334 asked for: write the incoming
 * image to the INACTIVE slot, verify it, point otadata at it, reboot into it --
 * no cable moved, no button, no download mode.  `bootloader` above is the other
 * half of that story: it is what is left when the new image does not come up.
 *
 * THE ARTIFACT IS THE APPLICATION .bin -- zimodem.ino.bin, about a megabyte --
 * and NOT the merged image.  The merged image carries a bootloader and a
 * partition table, and an OTA must never write those: they are what is doing the
 * writing, and the slot it writes into is an application slot.
 *
 * THE PROTOCOL IS ONE ACKNOWLEDGEMENT BYTE PER CHUNK, and that is not decoration.
 * The USB CDC's receive queue is 256 bytes deep (USBCDC.cpp -- the default in
 * setRxBufferSize), and a flash write stalls this loop for tens of milliseconds
 * while a 4 KB sector is erased: long enough for hundreds of bytes to arrive with
 * nowhere to go, and an overflowed queue drops them silently.  So the sender waits
 * for one byte per chunk and is therefore never more than one chunk ahead, and
 * while the flash is being written the sender is idle and the queue is empty.
 * The acknowledgement IS the progress -- '.' per chunk, and nothing else is
 * printed during a transfer, so a sender can count them.
 *
 * A FAILED TRANSFER IS NOT A BRICK.  Update.end() verifies the image before it
 * touches otadata, so a truncated or corrupted one is refused and the running
 * slot keeps running -- which is why every failure message here says so.
 */
#define O1NEXT_OTA_CHUNK 4096u          /* one flash sector: SPI_FLASH_SEC_SIZE */
#define O1NEXT_OTA_TIMEOUT_MS 5000u     /* per chunk, so a dead sender ends it */

/* Read up to `want` bytes, and give up if nothing arrives for the timeout.
 * Bounded on purpose: a sender that dies mid-transfer has to end the update
 * rather than park the modem in a receive loop forever.
 *
 * USBCDC::read() returns -1 as a size_t when the port is not open (see its own
 * source), which is SIZE_MAX by the time anything is added to it -- so the result
 * is checked against what was asked for, not just against zero. */
static size_t o1nextOtaReadChunk(uint8_t *buf, size_t want)
{
    size_t        got  = 0;
    unsigned long last = millis();

    while (got < want) {
        size_t n = Serial.read(buf + got, want - got);

        if (n > 0 && n <= (want - got)) {
            got += n;
            last = millis();
            continue;
        }
        if (millis() - last > O1NEXT_OTA_TIMEOUT_MS)
            break;
        delay(1);                       /* let the USB task refill the queue */
    }
    return got;
}
/* The image lands here a sector at a time, whichever way it arrives.  One buffer
 * for both feeders: they never run at the same time, and 4 KB of RAM is 4 KB. */
static uint8_t o1nextOtaBuf[O1NEXT_OTA_CHUNK];

/* The slot checks and Update.begin(), shared by both feeders.  This is what refuses
 * an image that cannot fit the inactive slot BEFORE a byte is written. */
static bool o1nextOtaPrepare(size_t size)
{
    if (!Update.begin(size)) {
        Serial.printf("\r\nupdate failed: %s\r\n", Update.errorString());
        return false;
    }
    return true;
}

/* Verify, and only then point otadata at the new slot.  Every failure leaves the
 * running slot running, which is why both feeders end here and not at
 * esp_ota_set_boot_partition(). */
static bool o1nextOtaFinish(void)
{
    if (!Update.end(true)) {
        Serial.printf("update failed: %s -- the running slot is untouched\r\n",
                      Update.errorString());
        return false;
    }
    Serial.print("update ok: written, verified, and otadata now points at it\r\n");
    Serial.flush();
    return true;
}

/* Receive `size` bytes of application image into the inactive slot.  Returns true
 * only when the image is written AND verified AND otadata points at it; the caller
 * then reboots into it.  False leaves the board exactly as it was. */
static bool o1nextOtaReceive(size_t size)
{
    uint8_t *chunk = o1nextOtaBuf;
    size_t   left  = size;
    size_t   chunks = 0;

    if (!o1nextOtaPrepare(size))
        return false;

    Serial.printf("\r\nready: send %u bytes in %u-byte chunks, one ack byte each\r\n",
                  (unsigned)size, (unsigned)O1NEXT_OTA_CHUNK);
    Serial.flush();

    while (left > 0) {
        size_t want = (left < sizeof(chunk)) ? left : sizeof(chunk);
        size_t got  = o1nextOtaReadChunk(chunk, want);

        if (got != want) {
            Update.abort();
            Serial.printf("\r\nupdate failed: %u of %u bytes of a chunk arrived in %u ms\r\n"
                          "        nothing was switched; the running slot is untouched\r\n",
                          (unsigned)got, (unsigned)want, (unsigned)O1NEXT_OTA_TIMEOUT_MS);
            return false;
        }
        if (Update.write(chunk, want) != want) {
            /* Read the reason BEFORE aborting: abort() overwrites it. */
            const char *why = Update.errorString();
            Update.abort();
            Serial.printf("\r\nupdate failed: %s after %u bytes\r\n"
                          "        nothing was switched; the running slot is untouched\r\n",
                          why, (unsigned)(size - left));
            return false;
        }
        left -= want;
        chunks++;
        Serial.write('.');              /* the acknowledgement, and the progress */
    }

    Serial.printf("\r\n%u bytes in %u chunks, verifying\r\n",
                  (unsigned)size, (unsigned)chunks);

    return o1nextOtaFinish();
}

/* `update` with no argument explains itself; with one, the argument is the size of
 * the image about to arrive.  The size is not a nicety: Update.begin() needs it to
 * refuse an image that cannot fit BEFORE a byte is written, and the sender needs
 * the board to agree before it starts pushing a megabyte. */
static void o1nextConsoleUpdate(const char *arg)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    size_t                 size   = 0;

    /* A URL is the other way in, and it is the one that needs no cable: the board
     * does the fetching. */
    if (arg != NULL && !strncmp(arg, "http://", 7)) {
        if (o1nextOtaFromUrl(arg)) {
            delay(100);
            esp_restart();
        }
        return;                         /* the console stays alive on a failure */
    }

    if (target == NULL) {
        Serial.print("\r\nupdate: there is no inactive slot to write to.\r\n");
        return;
    }

    if (arg == NULL || *arg == '\0') {
        Serial.printf("\r\nupdate: give it the size of the image in bytes -- `update <bytes>` --\r\n"
                      "        or a URL to fetch it from: `update http://host:port/file.bin`.\r\n"
                      "        the inactive slot is %s at 0x%06x, %u bytes.\r\n"
                      "        the artifact is zimodem.ino.bin, the application alone --\r\n"
                      "        not the merged image.  tools/update-o1next.py sends it,\r\n"
                      "        tools/ota-server-o1next.py serves it.\r\n",
                      target->label, (unsigned)target->address, (unsigned)target->size);
        return;
    }

    for (const char *p = arg; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            Serial.printf("\r\nupdate: '%s' is not a number of bytes.\r\n", arg);
            return;
        }
        if (size > 0x4000000u) {        /* larger than any slot on this part */
            Serial.print("\r\nupdate: that size is not an image this board could hold.\r\n");
            return;
        }
        size = size * 10 + (size_t)(*p - '0');
    }

    if (size == 0) {
        Serial.print("\r\nupdate: zero bytes is not an image.\r\n");
        return;
    }
    if (size > target->size) {
        Serial.printf("\r\nupdate: %u bytes will not fit in %s, which is %u bytes.\r\n",
                      (unsigned)size, target->label, (unsigned)target->size);
        return;
    }

    if (!o1nextOtaReceive(size))
        return;                         /* the console stays alive, and so does the board */

    /* The image is in the inactive slot, verified, and otadata points at it: this
     * is the moment to be running it rather than talking about it.  The delay is
     * for the host to read the line above before the port goes away. */
    delay(100);
    esp_restart();
}



/* -------------------------------------------------------------- the network --
 *
 * Three things that only exist because the board can be on a network: joining one,
 * fetching an image over it, and arming a fetch for the next boot.
 *
 * The modem's own AT command set already configures the network -- AT*SSID and
 * AT*PASS, over the O1 side -- and that is the path a user in the field uses.  But
 * the O1 side needs a machine on the other end of it, and a board on a bench has
 * none.  So the console can do it too, and it saves through the SAME config file
 * (commandMode.reSaveConfig()) so the two front ends cannot end up disagreeing
 * about which network this board is on.
 */
static void o1nextConsoleWifi(const char *arg)
{
    const char *sp;
    String      ssid;
    String      pw;

    if (arg == NULL || *arg == '\0') {
        Serial.printf("\r\nwifi: stored ssid is \"%s\"\r\n", wifiSSI.c_str());
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("      connected to \"%s\", ip %s, gw %s\r\n",
                          WiFi.SSID().c_str(),
                          WiFi.localIP().toString().c_str(),
                          WiFi.gatewayIP().toString().c_str());
            Serial.printf("      rssi %d dBm, channel %d -- the S2 has a 2.4 GHz radio only\r\n",
                          WiFi.RSSI(), WiFi.channel());
        } else
            Serial.print("      not connected\r\n");
        return;
    }

    sp = strchr(arg, ' ');
    if (sp == NULL) {
        Serial.print("\r\nwifi: it wants both: `wifi <ssid> <password>`\r\n");
        return;
    }
    ssid = String(arg).substring(0, sp - arg);
    pw   = String(sp + 1);
    ssid.trim();
    pw.trim();
    if (ssid.length() == 0) {
        Serial.print("\r\nwifi: an empty ssid is not a network.\r\n");
        return;
    }

    wifiSSI = ssid;
    wifiPW  = pw;

    Serial.printf("\r\nwifi: joining \"%s\"\r\n", wifiSSI.c_str());
    if (!connectWifi(wifiSSI.c_str(), wifiPW.c_str(), staticIP, staticDNS, staticGW, staticSN)) {
        Serial.print("      did not connect -- a wrong password, or a network this board\r\n"
                     "      cannot see: the ESP32-S2 has a 2.4 GHz radio and no other.\r\n"
                     "      Nothing was saved.\r\n");
        return;
    }

    Serial.printf("      connected: ip %s, gw %s, rssi %d dBm, channel %d\r\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.RSSI(), WiFi.channel());

    if (commandMode.reSaveConfig(3))
        Serial.print("      saved -- it will rejoin this network on the next boot\r\n");
    else
        Serial.print("      WARNING: connected, but the config would not save\r\n");
}

/* -------------------------------------------------------- update over HTTP --
 *
 * `update http://host:port/path` -- the image arrives over the network instead of
 * down this console.  Plain HTTP, and the scheme is checked rather than assumed:
 * there is no trust store on this board, and an update over an unverified
 * connection is not a thing to do quietly.
 *
 * What it needs from the server, and why the listener on the other end is built to
 * provide it: a **Content-Length**.  Update.begin(size) wants the size before a
 * byte is written -- it is what refuses an image that cannot fit the slot -- so a
 * chunked answer is refused here rather than guessed at.
 *
 * The body is streamed 4 KB at a time from the socket into the flash.  Nothing
 * holds the image in RAM, which is the whole reason this is a loop.
 *
 * The same o1nextOtaPrepare()/o1nextOtaFinish() pair the console feeder uses, so
 * both ways in verify before otadata moves and both fail the same way.
 */
static bool o1nextOtaFromUrl(const char *url)
{
    WiFiClient    client;
    String        hostport;
    String        path = "/";
    char          host[64];
    int           port       = 80;
    size_t        size       = 0;
    int           status     = 0;
    unsigned long started;
    unsigned long lastPrint;
    size_t        left;

    if (WiFi.status() != WL_CONNECTED) {
        Serial.print("\r\nupdate: this board is not on a network yet.  `wifi <ssid> <password>`\r\n"
                     "        first, or `wifi` on its own to see what it is holding.\r\n");
        return false;
    }

    {
        const char *p     = url + 7;                /* past "http://" */
        const char *slash = strchr(p, '/');
        int         colon;

        hostport = slash ? String(p).substring(0, slash - p) : String(p);
        if (slash != NULL)
            path = String(slash);

        colon = hostport.indexOf(':');
        if (colon >= 0) {
            port     = hostport.substring(colon + 1).toInt();
            hostport = hostport.substring(0, colon);
        }
        if (hostport.length() == 0 || port <= 0 || port > 65535) {
            Serial.printf("\r\nupdate: '%s' is not a URL this can fetch.\r\n", url);
            return false;
        }
        hostport.toCharArray(host, sizeof(host));
    }

    Serial.printf("\r\nupdate: GET http://%s:%d%s\r\n", host, port, path.c_str());
    if (!client.connect(host, port, 10000)) {
        Serial.printf("        could not reach %s:%d\r\n", host, port);
        return false;
    }
    client.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                  path.c_str(), host);
    client.setTimeout(10000);

    while (client.connected() || client.available()) {
        String line = client.readStringUntil('\n');

        line.trim();
        if (line.length() == 0)
            break;                                  /* end of the head */
        if (status == 0) {
            int first = line.indexOf(' ');
            status = (first > 0) ? line.substring(first + 1).toInt() : -1;
            continue;
        }
        line.toLowerCase();
        if (line.startsWith("content-length:"))
            size = (size_t)line.substring(15).toInt();
    }

    if (status != 200) {
        Serial.printf("        the server answered %d, not 200 -- nothing was written\r\n", status);
        client.stop();
        return false;
    }
    if (size == 0) {
        Serial.print("        no Content-Length in the answer, so there is no size to give\r\n"
                     "        Update.begin() -- nothing was written\r\n");
        client.stop();
        return false;
    }

    if (!o1nextOtaPrepare(size)) {
        client.stop();
        return false;
    }

    started   = millis();
    lastPrint = started;
    left      = size;

    while (left > 0) {
        size_t        want   = (left < sizeof(o1nextOtaBuf)) ? left : sizeof(o1nextOtaBuf);
        size_t        got    = 0;
        unsigned long waited = millis();

        while (got < want) {
            if (client.available()) {
                got += client.read(o1nextOtaBuf + got, want - got);
                waited = millis();
                continue;
            }
            if (!client.connected())
                break;
            if (millis() - waited > 15000)
                break;
            delay(1);
        }

        if (got != want) {
            Update.abort();
            Serial.printf("\r\nupdate failed: the connection %s after %u of %u bytes\r\n"
                          "        nothing was switched; the running slot is untouched\r\n",
                          (got == 0) ? "stalled" : "ended early",
                          (unsigned)(size - left), (unsigned)size);
            client.stop();
            return false;
        }
        if (Update.write(o1nextOtaBuf, want) != want) {
            const char *why = Update.errorString();   /* read it before abort() */
            Update.abort();
            Serial.printf("\r\nupdate failed: %s after %u bytes\r\n"
                          "        nothing was switched; the running slot is untouched\r\n",
                          why, (unsigned)(size - left));
            client.stop();
            return false;
        }
        left -= want;

        /* Progress, and only for a host that is listening: with nobody attached
         * these writes are no-ops, and on a headless board that is the difference
         * between an update that takes seconds and one that crawls. */
        if (Serial && (millis() - lastPrint > 1000)) {
            Serial.printf("\r        %u of %u bytes", (unsigned)(size - left), (unsigned)size);
            lastPrint = millis();
        }
    }
    client.stop();

    Serial.printf("\r\n        %u bytes in %u ms\r\n",
                  (unsigned)size, (unsigned)(millis() - started));

    return o1nextOtaFinish();
}

/* ------------------------------------------------------- update-at-boot --
 *
 * One URL, remembered, fetched once at the next boot.  This is what makes an update
 * need no cable at all: arm it while the console is attached, then power the board
 * from anything, and it updates itself and reboots into the new image.
 *
 * The URL is cleared only after a fetch that verified, and before the reboot.  So a
 * board that cannot reach the network keeps the instruction and tries again next
 * boot, and a board that succeeds does not loop.
 */
#define O1NEXT_UPDATE_URL_FILE "/o1next-update.txt"

static void o1nextConsoleUpdateAtBoot(const char *arg)
{
    File f;

    if (arg == NULL || *arg == '\0') {
        if (!SPIFFS.exists(O1NEXT_UPDATE_URL_FILE)) {
            Serial.print("\r\nupdate-at-boot: nothing armed.\r\n");
            return;
        }
        f = SPIFFS.open(O1NEXT_UPDATE_URL_FILE, "r");
        if (!f) {
            Serial.print("\r\nupdate-at-boot: armed, but the file will not open.\r\n");
            return;
        }
        String url = f.readStringUntil('\n');
        f.close();
        url.trim();
        Serial.printf("\r\nupdate-at-boot: armed for \"%s\"\r\n", url.c_str());
        return;
    }

    if (!strcmp(arg, "off")) {
        SPIFFS.remove(O1NEXT_UPDATE_URL_FILE);
        Serial.print("\r\nupdate-at-boot: disarmed.\r\n");
        return;
    }
    if (strncmp(arg, "http://", 7) != 0) {
        Serial.print("\r\nupdate-at-boot: it wants an http:// URL, or `off`.\r\n");
        return;
    }

    SPIFFS.remove(O1NEXT_UPDATE_URL_FILE);
    f = SPIFFS.open(O1NEXT_UPDATE_URL_FILE, "w");
    if (!f) {
        Serial.print("\r\nupdate-at-boot: could not write it to the filesystem.\r\n");
        return;
    }
    f.printf("%s\n", arg);
    f.close();
    Serial.printf("\r\nupdate-at-boot: armed for \"%s\"\r\n"
                  "        the next boot fetches it once, over the network, with nothing\r\n"
                  "        plugged in -- and `update-at-boot off` cancels it.\r\n", arg);
}

/* Once per boot, from the console poll. */
static void o1nextUpdateAtBootRun(void)
{
    String url;
    File   f;
    int    i;

    if (!SPIFFS.exists(O1NEXT_UPDATE_URL_FILE))
        return;
    f = SPIFFS.open(O1NEXT_UPDATE_URL_FILE, "r");
    if (!f)
        return;
    url = f.readStringUntil('\n');
    f.close();
    url.trim();
    if (url.length() == 0) {
        SPIFFS.remove(O1NEXT_UPDATE_URL_FILE);
        return;
    }

    Serial.printf("\r\nupdate-at-boot: armed for \"%s\"\r\n", url.c_str());

    /* The modem's own boot-time connect has already had its turn -- loadConfig()
     * blocks while it connects -- so this wait is only for a board that came up
     * before its access point did.  Bounded, because the instruction survives a
     * failed attempt: it is cleared only by a fetch that verified. */
    for (i = 0; (i < 60) && (WiFi.status() != WL_CONNECTED); i++)
        delay(500);

    if (WiFi.status() != WL_CONNECTED) {
        Serial.print("        no network yet -- still armed, and it will try next boot\r\n");
        return;
    }
    if (!o1nextOtaFromUrl(url.c_str())) {
        Serial.print("        still armed, and it will try next boot\r\n");
        return;
    }

    SPIFFS.remove(O1NEXT_UPDATE_URL_FILE);       /* one shot: cleared before the reboot */
    Serial.print("        done -- disarmed, and rebooting into it\r\n");
    delay(200);
    esp_restart();
}

/* --------------------------------------------------------------------- help --
 *
 * A console with no help is a guessing game.  `bootloader` gets a sentence because
 * it is the one whose mechanism is not guessable, and `update` because it has a
 * protocol a sender has to match.
 */
static void o1nextConsoleHelp(void)
{
    Serial.print(
        "\r\ncommands:\r\n"
        "  status      version, pins in use, the running slot, uptime, free heap\r\n"
        "  wifi        the network: `wifi` reports it, `wifi <ssid> <password>`\r\n"
        "              joins one and saves it.  2.4 GHz only -- that is the radio.\r\n"
        "  update <n>  take <n> bytes on this console into the inactive slot,\r\n"
        "              verify it, and reboot into it.  tools/update-o1next.py sends\r\n"
        "              it, and the size is what refuses an image that cannot fit.\r\n"
        "  update <url>  fetch the image over HTTP instead: the board does the work,\r\n"
        "              the network carries it, and nothing is sent down this port.\r\n"
        "  update-at-boot <url>|off  arm one fetch for the next boot -- an update\r\n"
        "              with no cable at all.\r\n"
        "  reboot      esp_restart() -- back into the application\r\n"
        "  bootloader  force the ROM's download mode and reset -- no button.  On a\r\n"
        "              board with no auto-reset circuit -- ours -- this is the only\r\n"
        "              way back to a flash tool.\r\n"
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
    char *arg = NULL;

    while (*cmd == ' ' || *cmd == '\t')
        cmd++;

    /* Split the argument off FIRST, then fold the case of the command word only.
     * The command should not fail on shift, but an argument has to arrive exactly
     * as typed: a URL's path is case-sensitive, and so is a wifi password -- and
     * lowercasing those was a bug this found rather than a nicety. */
    for (char *p = cmd; *p != '\0'; p++) {
        if (*p == ' ' || *p == '\t') {
            *p  = '\0';
            arg = p + 1;
            while (*arg == ' ' || *arg == '\t')
                arg++;
            if (*arg == '\0')
                arg = NULL;
            break;
        }
    }

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
    else if (!strcmp(cmd, "wifi"))
        o1nextConsoleWifi(arg);
    else if (!strcmp(cmd, "update"))
        o1nextConsoleUpdate(arg);
    else if (!strcmp(cmd, "update-at-boot"))
        o1nextConsoleUpdateAtBoot(arg);
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
    static bool lookedForAnArmedUpdate = false;
    int c;

    o1nextConsoleGreet();

    /* The armed one-shot, once per boot.  Here rather than in setup() because it
     * prints, and because loop() is where this file already lives. */
    if (!lookedForAnArmedUpdate) {
        lookedForAnArmedUpdate = true;
        o1nextUpdateAtBootRun();
    }

    while ((c = Serial.read()) >= 0)
    {
        if (c == '\r' || c == '\n')
        {
            /* A terminal sends CRLF, and that is one line rather than a line and
             * an empty one -- so a terminator is swallowed only when it is the
             * OTHER one from the one that just ended a line.
             *
             * Not pedantry, and not what this did first: "a terminator that
             * follows a terminator is not a new line" also swallows a bare CR sent
             * to ask for a prompt, so pressing Enter after a command printed
             * nothing at all.  Measured, by tools/update-o1next.py, which asks for
             * a prompt exactly that way.  Two Enters are two lines and both get a
             * prompt; CRLF is one line. */
            if (o1nextConsoleLastTerm != 0 && c != o1nextConsoleLastTerm)
            {
                o1nextConsoleLastTerm = 0;
                continue;
            }
            o1nextConsoleLastTerm = (char)c;

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

        o1nextConsoleLastTerm = 0;

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

