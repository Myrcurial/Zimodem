#!/usr/bin/env python3
"""Type at the O1NEXT console: open the port, send commands, print what comes back.

The counterpart to read-console.py, which only listens.  This one writes, which
is what makes the console's commands reachable from a script -- including
`bootloader`, which is the one worth scripting: put the board in the ROM's
download mode with no button, then flash it.

    tools/console-o1next.py '/dev/cu.usbmodem*' status
    tools/console-o1next.py '/dev/cu.usbmodem*' status help
    tools/console-o1next.py '/dev/cu.usbmodem*' bootloader   # then flash

The port argument may be a glob, and the newest match wins on every (re)open --
the re-enumerated device lands on a different name, and `reboot` and `bootloader`
both reset the chip, so a reader that opens once misses everything after the
command it was run to send.  Measured, in read-console.py's company and for the
same reason.

Two things it does that a plain `cat` does not, and both were learned the hard
way while testing #334:

  - It sends a bare CR as the line terminator, which is what the console expects;
    the console treats a following LF as the same line rather than a second one.
  - It keeps reading for a while after the last command, because `reboot` and
    `bootloader` take the port away and bring it back -- and the boot banner
    after a `reboot` is the evidence that the command worked.

Environment: CONSOLE_SECONDS (default 30), CONSOLE_GAP (seconds between commands,
default 2.0).

One caveat that is not a bug: if esptool has just been talking to a *running*
application (which is what `--before no_reset` does when the chip is not in
download mode), it has filled the console's line buffer with its sync filler --
0x55, which is 'U', and printable -- so the console will answer `line too long`
to the first command sent afterwards.  Send a bare newline first, or ignore the
one lost line.  See #334.
"""
import glob
import os
import select
import sys
import termios
import time
import tty

if len(sys.argv) < 3:
    sys.stderr.write("usage: console-o1next.py <port-or-glob> <command> [command...]\n")
    sys.exit(2)

pattern = sys.argv[1]
commands = sys.argv[2:]
seconds = float(os.environ.get("CONSOLE_SECONDS", "30"))
gap = float(os.environ.get("CONSOLE_GAP", "2.0"))


def resolve():
    """The newest path matching the pattern, or None."""
    if any(c in pattern for c in "*?["):
        matches = sorted(glob.glob(pattern), key=lambda p: -os.stat(p).st_mtime)
        return matches[0] if matches else None
    return pattern if os.path.exists(pattern) else None


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        tty.setraw(fd)
        termios.tcsetattr(fd, termios.TCSANOW, termios.tcgetattr(fd))
    except termios.error:
        pass
    return fd


def close(fd):
    try:
        os.close(fd)
    except OSError:
        pass


deadline = time.time() + seconds
fd = None
sent = 0
opens = 0
next_send = time.time() + 3.0        # give the host a moment to be seen first

while time.time() < deadline:
    if fd is None:
        path = resolve()
        if path is None:
            time.sleep(0.05)
            continue
        try:
            fd = open_port(path)
        except OSError:
            fd = None
            time.sleep(0.05)
            continue
        opens += 1
        if opens > 1:
            sys.stderr.write("\n[re-opened %s]\n" % path)

    try:
        r, _, _ = select.select([fd], [], [], 0.25)
        data = os.read(fd, 4096) if r else b""
    except OSError:
        # The device went away underneath us -- almost always a reset.  Close,
        # and let the loop re-open: that is what `reboot` and `bootloader` do.
        close(fd)
        fd = None
        continue

    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()

    if sent < len(commands) and time.time() >= next_send:
        try:
            os.write(fd, (commands[sent] + "\r").encode())
            sys.stderr.write("\n[sent: %s]\n" % commands[sent])
            sent += 1
            next_send = time.time() + gap
        except OSError:
            close(fd)
            fd = None

if fd is not None:
    close(fd)

sys.stderr.write("\n[console-o1next: sent %d/%d commands, %d port opens]\n"
                 % (sent, len(commands), opens))

