#!/usr/bin/env python3
"""Send an application image to the O1NEXT console's `update` command.

    tools/update-o1next.py '/dev/cu.usbmodem*' build/o1next-s2/zimodem.ino.bin

WHAT IT SENDS, AND WHAT IT DOES NOT

The artifact is the **application** image -- `zimodem.ino.bin`, about a megabyte --
and not `zimodem-o1next-merged.bin`.  The merged image carries a bootloader and a
partition table, and an OTA must never write those: they are what is doing the
writing.  The firmware's own `update` command says the same thing if you give it no
size.

THE PROTOCOL, because a sender has to match it exactly

    -> update <bytes>\\r
    <- ready: send <bytes> bytes in <chunk>-byte chunks, one ack byte each\\r\\n
    -> <chunk bytes>          (repeated; the last chunk is whatever is left)
    <- .                      (exactly one byte per chunk, and nothing else)

The acknowledgement is one byte per chunk and it is load-bearing rather than
politeness.  The device's USB CDC receive queue is 256 bytes deep and a flash write
stalls its loop for tens of milliseconds, so a sender that pushes a megabyte as
fast as the port takes it overruns that queue and the bytes are dropped silently.
Waiting for one byte per chunk means this end is never more than one chunk ahead,
and while the flash is being written this end is idle.

The device verifies the image before it switches anything: `Update.end()` checks it
and only then points otadata at the new slot.  So a truncated or corrupted transfer
is refused and the running slot keeps running -- and this script reports which of
the two happened, from the device's own words.

AFTERWARDS

On success the board reboots into the new image by itself, which takes the port
away and brings it back.  This waits for it and then asks the new image `status`,
so the last thing printed is the evidence that the update took: the slot it is
running from, and the time it was built.
"""
import glob
import hashlib
import os
import select
import sys
import termios
import time
import tty

CHUNK = 4096                 # one flash sector; the firmware's O1NEXT_OTA_CHUNK
ACK_TIMEOUT = 20.0           # seconds to wait for one ack byte
READY_TIMEOUT = 60.0         # seconds for the board to erase the slot and agree
REBOOT_TIMEOUT = 30.0        # seconds to wait for the new image to come up
PROGRESS_EVERY = 32          # chunks between progress lines (32 x 4 KB = 128 KB)


def die(msg):
    sys.stderr.write("update-o1next: %s\n" % msg)
    sys.exit(1)


def note(msg):
    sys.stderr.write("%s\n" % msg)
    sys.stderr.flush()


class Port:
    """The console port, which re-opens itself.

    The reopen rule is read-console.py's, for the same measured reason: resetting
    the chip changes the USB device, so the handle dies and the port can come back
    under a different name.  An update ends in exactly that reset.
    """

    def __init__(self, pattern):
        self.pattern = pattern
        self.fd = None
        self.opens = 0

    def _path(self):
        if any(c in self.pattern for c in "*?["):
            matches = sorted(glob.glob(self.pattern),
                             key=lambda p: -os.stat(p).st_mtime)
            return matches[0] if matches else None
        return self.pattern if os.path.exists(self.pattern) else None

    def open(self, timeout=15.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            path = self._path()
            if path is not None:
                try:
                    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                except OSError:
                    time.sleep(0.05)
                    continue
                try:
                    tty.setraw(fd)
                    termios.tcsetattr(fd, termios.TCSANOW, termios.tcgetattr(fd))
                except termios.error:
                    pass
                self.fd = fd
                self.opens += 1
                if self.opens > 1:
                    note("[re-opened %s]" % path)
                return True
            time.sleep(0.05)
        return False

    def close(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None

    def read(self, timeout=0.2):
        """Whatever arrived, or b'' -- and close if the device went away."""
        if self.fd is None:
            return b""
        try:
            r, _, _ = select.select([self.fd], [], [], timeout)
            if not r:
                return b""
            return os.read(self.fd, 4096)
        except OSError:
            self.close()
            return b""

    def write(self, data, timeout=10.0):
        """All of it, or False.  A tty takes a chunk in pieces, so this loops."""
        if self.fd is None:
            return False
        deadline = time.time() + timeout
        view = memoryview(data)
        while len(view) > 0:
            try:
                _, w, _ = select.select([], [self.fd], [], 0.5)
                if not w:
                    if time.time() > deadline:
                        return False
                    continue
                n = os.write(self.fd, view)
                view = view[n:]
            except OSError:
                self.close()
                return False
        return True


def wait_until(port, markers, timeout, echo=True):
    """Read until one of `markers` appears; return (text, the marker or None).

    More than one marker because the board answers a command with either its
    acceptance or its refusal, and waiting for a newline instead returns on the
    *echo* of the command -- which is a line too, and arrives first.
    """
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if port.fd is None:
            # The device went away -- mid-reset, most likely, and an update ends in
            # one.  Reopen and keep waiting rather than give up: this is
            # read-console.py's rule, for read-console.py's reason.  It also covers
            # landing on the node that is on its way out, whose reads fail.
            port.open(timeout=min(2.0, max(0.05, deadline - time.time())))
            if port.fd is None:
                continue
        data = port.read(0.2)
        if data:
            buf += data
            if echo:
                sys.stdout.write(data.decode("latin-1"))
                sys.stdout.flush()
            for marker in markers:
                if marker in buf:
                    return buf.decode("latin-1", "replace"), marker
    return buf.decode("latin-1", "replace"), None


def wait_for(port, marker, timeout, echo=True):
    """One marker; returns (text, found) rather than the marker."""
    text, found = wait_until(port, (marker,), timeout, echo)
    return text, found is not None


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: update-o1next.py <port-or-glob> <app.bin>\n")
        sys.exit(2)

    pattern, image = sys.argv[1], sys.argv[2]
    try:
        with open(image, "rb") as fh:
            blob = fh.read()
    except OSError as exc:
        die("cannot read %s: %s" % (image, exc))
    if len(blob) == 0:
        die("%s is empty" % image)

    size = len(blob)
    chunks = (size + CHUNK - 1) // CHUNK

    note("== image    %s" % image)
    note("==          %d bytes in %d chunks of %d" % (size, chunks, CHUNK))
    note("== sha256   %s" % hashlib.sha256(blob).hexdigest())

    port = Port(pattern)
    if not port.open():
        die("no port matching %s -- is the board connected?" % pattern)

    # The console greets a host that is listening, and it may have greeted long ago
    # and gone quiet: a newline is how to ask a prompt of it.  Asked more than once
    # because the first can be eaten -- by a board that has just reset, or by a line
    # buffer still full of esptool's sync filler (see the note at the top).
    found = False
    for _ in range(3):
        _, found = wait_for(port, b"o1next>", 3.0)
        if found:
            break
        port.write(b"\r")
    if not found:
        die("no console prompt -- is the application running, and the port right?")

    note("== offering %d bytes to the board" % size)
    if not port.write(("update %d\r" % size).encode()):
        die("could not send the update command")

    # The board erases the inactive slot before it says ready -- esp_ota_begin()
    # erases the whole partition, and 1.875 MB of flash takes seconds -- so this
    # waits for the word rather than for a line.
    text, marker = wait_until(port, (b"ready:", b"update:"), READY_TIMEOUT)
    if marker != b"ready:":
        die("the board did not agree to receive: %s" % text.strip())

    started = time.time()
    sent = 0
    for i in range(chunks):
        piece = blob[sent:sent + CHUNK]
        if not port.write(piece, timeout=ACK_TIMEOUT):
            die("the port went away sending chunk %d of %d" % (i + 1, chunks))

        # One ack byte per chunk.  Nothing else is printed during a transfer, so
        # anything that is not a '.' is the board objecting.
        acked = b""
        deadline = time.time() + ACK_TIMEOUT
        while time.time() < deadline:
            data = port.read(0.2)
            if data:
                acked = data
                break
            if port.fd is None:
                die("the port went away waiting for the ack of chunk %d" % (i + 1))
        if not acked:
            die("no ack for chunk %d of %d in %.0f s -- the board gave up first"
                % (i + 1, chunks, ACK_TIMEOUT))
        if acked != b".":
            sys.stdout.write(acked.decode("latin-1"))
            sys.stdout.flush()

        sent += len(piece)
        if (i + 1) % PROGRESS_EVERY == 0 or i + 1 == chunks:
            note("   %d/%d chunks, %d of %d bytes, %.1f s"
                 % (i + 1, chunks, sent, size, time.time() - started))

    # ---- the verdict ----
    # The board verifies the image before it touches otadata, so this is where a
    # corrupted transfer is caught -- and where "the running slot is untouched" is
    # known to be true, because the board says so.
    buf  = b""
    ok   = None
    deadline = time.time() + 30.0
    while time.time() < deadline and ok is None:
        data = port.read(0.3)
        if data:
            sys.stdout.write(data.decode("latin-1"))
            sys.stdout.flush()
            buf += data
            if b"update ok" in buf:
                ok = True
            elif b"update failed" in buf:
                ok = False
        elif port.fd is None:
            break

    if ok is None:
        die("no verdict from the board -- see what it said above")
    if not ok:
        die("the board refused the image; the running slot is untouched")

    # ---- and the new image, running ----
    note("== the board is rebooting into it")
    port.close()
    if not port.open(timeout=REBOOT_TIMEOUT):
        die("the port did not come back after the update")

    _, found = wait_for(port, b"o1next>", 12.0)
    if not found:
        port.write(b"\r")
        _, found = wait_for(port, b"o1next>", 8.0)
    if not found:
        die("the new image did not answer on the console")

    note("== asking the new image what it is")
    port.write(b"status\r")
    wait_for(port, b"o1next>", 10.0)
    port.close()

    note("== done -- the slot and the build time above are the new image")


if __name__ == "__main__":
    main()
