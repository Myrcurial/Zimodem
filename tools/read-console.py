#!/usr/bin/env python3
"""Read a serial port for N seconds and print what arrives.

Opens the port in raw mode so the tty layer does not translate or echo, and
polls with select() so a quiet line still returns when the time is up.

It RE-OPENS on error, and that is the whole point rather than a nicety.  On this
board the console port disappears and comes back whenever the chip resets --
resetting running firmware into the ROM's download mode changes the USB device,
and so does the other direction -- so a reader that opens once and gives up on
the first ENXIO misses every boot banner it was run to catch.  Measured, twice.

The port argument may be a glob; on every (re)open the newest match wins, because
the re-enumerated device can land on a different name.
"""
import glob
import os
import select
import sys
import termios
import time
import tty

if len(sys.argv) < 3:
    sys.stderr.write("usage: read-console.py <port-or-glob> <seconds>\n")
    sys.exit(2)

pattern = sys.argv[1]
seconds = float(sys.argv[2])


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


deadline = time.time() + seconds
fd = None
path = None
chunks = 0
total = 0
reopens = 0

while time.time() < deadline:
    if fd is None:
        path = resolve()
        if path is None:
            time.sleep(0.05)
            continue
        try:
            fd = open_port(path)
            if reopens:
                sys.stderr.write("[read-console: re-opened %s]\n" % path)
        except OSError:
            fd = None
            time.sleep(0.05)
            continue

    try:
        r, _, _ = select.select([fd], [], [], 0.5)
        if not r:
            continue
        data = os.read(fd, 4096)
    except OSError:
        # The device went away underneath us -- almost always a reset.  Close,
        # count it, and let the loop re-open.
        try:
            os.close(fd)
        except OSError:
            pass
        fd = None
        reopens += 1
        continue

    if not data:
        continue
    chunks += 1
    total += len(data)
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

if fd is not None:
    try:
        os.close(fd)
    except OSError:
        pass

sys.stderr.write("\n[read-console: %d bytes in %d chunks, %d re-opens]\n"
                 % (total, chunks, reopens))
