/*
   o1next_console.h -- the O1NEXT board's console, declared for zimodem.ino.

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

   The implementation is o1next_console.ino, which Arduino concatenates into the
   same translation unit as zimodem.ino -- so this header exists for exactly one
   reason: loop() calls into the console, and a declaration is better than
   depending on the prototypes Arduino generates.  Upstream's own .h/.ino pairs
   (proto_ftp, zcommand, ...) are the same shape.

   The console is the native USB CDC -- it is NOT the modem's serial port.  The
   O1 side is UART1 (GPIO15/16) and the ROM's own log is UART0, and nothing in
   the console reads or writes either: it cannot consume a byte the Osborne sent.

   The guard is the same one zimodem.ino uses for the CDC half of debugPrintf,
   because it is the same fact -- with CDCOnBoot=cdc the USB port is the console,
   and without it this board has no console at all.
*/

#ifndef O1NEXT_CONSOLE_H
#define O1NEXT_CONSOLE_H

#if defined(O1NEXT_BOARD) && ARDUINO_USB_CDC_ON_BOOT

/* Read whatever the host has typed, run a whole line if there is one, return.
 * Called from loop(); it never blocks on input, and it never waits on a line. */
void o1nextConsolePoll(void);

#endif /* O1NEXT_BOARD && ARDUINO_USB_CDC_ON_BOOT */

#endif /* O1NEXT_CONSOLE_H */
