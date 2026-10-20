#!/usr/bin/env python3
"""Which board is which: every serial port here, what is behind it, and its MAC.

Two ESP32-S2 boards enumerate as `/dev/cu.usbmodem*`, and the number in that name
changes when a board resets -- so the port name is not an identity.  The MAC is: it is
burned into the eFuse, it survives everything, and our firmware prints it.

    tools/identify-o1next.py                                  # a table
    tools/identify-o1next.py --json                           # for a script
    tools/identify-o1next.py --find-mac 80:65:99:ef:c7:82     # just the port

Each port is asked three ways, gentlest first:

  1. **the o1next console** -- a bare CR, and if a prompt answers, `status`.  Names the
     board, its MAC, its build time and its slot, and disturbs nothing.
  2. **esptool read_mac** -- answers for *any* ESP32, firmware or not, at the price of
     resetting it (it comes back running whatever it was running).
  3. **nothing** -- which is the answer for a USB-TTL adapter: a CP2102/CH340/FT232 is a
     serial bridge, not a chip esptool can sync with.  That is useful to know too, and
     the tool says so rather than leaving a blank.

The MAC in the table is the thing to write on the board with a marker.  A label plus
this tool is the whole answer to "which one is which"; everything else is convenience.
"""
import glob
import json
import os
import re
import select
import shutil
import subprocess
import sys
import termios
import time
import tty

PORT_GLOBS = [
    "/dev/cu.usbmodem*",        # the ESP32-S2's native USB
    "/dev/cu.wchusbserial*",    # CH340
    "/dev/cu.usbserial*",       # FT232 / CP2102
    "/dev/cu.SLAB_USBtoUART*",  # CP210x, Silicon Labs driver
    "/dev/cu.usbmodem*",
    "/dev/ttyACM*",
    "/dev/ttyUSB*",
]

MAC_RE = re.compile(r"([0-9a-f]{2}(?::[0-9a-f]{2}){5})", re.I)
CONSOLE_PROMPT = b"o1next> "


def ports():
    """Every candidate serial port, de-duplicated and in a stable order."""
    found = []
    for pattern in PORT_GLOBS:
        for path in glob.glob(pattern):
            if path not in found:
                found.append(path)
    return sorted(found)


def find_esptool():
    """The esptool the Arduino core ships, the same one the build scripts use."""
    cli = shutil.which("arduino-cli")
    if cli:
        try:
            data = subprocess.run([cli, "config", "get", "directories.data"],
                                  capture_output=True, text=True, timeout=15)
            root = data.stdout.strip().strip('"')
            for candidate in sorted(glob.glob(
                    os.path.join(root, "packages/esp32/tools/esptool_py/*/esptool")),
                    reverse=True):
                if os.access(candidate, os.X_OK):
                    return candidate
        except (OSError, subprocess.SubprocessError):
            pass
    return shutil.which("esptool")           # or nothing, and the tool says so


def human_age(seconds):
    if seconds < 90:
        return "%.0f s" % seconds
    if seconds < 5400:
        return "%.1f min" % (seconds / 60.0)
    return "%.1f h" % (seconds / 3600.0)


def ask_console(port, timeout=1.2):
    """Ask a board's console who it is.  Returns a dict, or None if nothing answers.

    Non-destructive: a bare CR and a `status`.  (A bare CR works because of the
    terminator fix in #334 -- before it, asking this way printed nothing at all.)
    """
    fd = None
    try:
        fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(fd)
        termios.tcsetattr(fd, termios.TCSANOW, termios.tcgetattr(fd))
    except OSError:
        if fd is not None:
            os.close(fd)
        return None

    out = b""

    def drain(seconds):
        nonlocal out
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                ready, _, _ = select.select([fd], [], [], 0.1)
                if not ready:
                    continue
                chunk = os.read(fd, 4096)
            except OSError:
                return False
            if not chunk:
                return False
            out += chunk
        return True

    try:
        os.write(fd, b"\r")
        drain(0.4)
        if CONSOLE_PROMPT not in out:
            return None
        os.write(fd, b"status\r")
        drain(timeout)
    except OSError:
        return None
    finally:
        try:
            os.close(fd)
        except OSError:
            pass

    text = out.decode("latin-1", "replace")
    result = {"how": "console", "mac": None, "built": None, "slot": None, "pins": None}

    match = MAC_RE.search(text)
    if match:
        result["mac"] = match.group(1).lower()
    match = re.search(r"\(built ([^)]+)\)", text)
    if match:
        result["built"] = match.group(1)
    match = re.search(r"running (\w+) at 0x([0-9a-f]+)", text)
    if match:
        result["slot"] = "%s at 0x%s" % (match.group(1), match.group(2))
    match = re.search(r"RXD (\d+), TXD (\d+)", text)
    if match:
        result["pins"] = "RXD %s, TXD %s" % (match.group(1), match.group(2))
    result["role"] = "o1next firmware (a console that answers `status`)"
    return result


def ask_esptool(esptool, port, chip="esp32s2", timeout=25):
    """Ask the ROM.  Works on a bare board; resets the chip, which comes back as it was."""
    if not esptool:
        return None
    try:
        done = subprocess.run(
            [esptool, "--chip", chip, "--port", port, "--before", "default_reset",
             "--after", "hard_reset", "read_mac"],
            capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return None
    text = (done.stdout or "") + (done.stderr or "")
    match = MAC_RE.search(text)
    if not match:
        return None
    chip_line = ""
    for line in text.splitlines():
        if line.startswith("Chip is "):
            chip_line = line.strip()
            break
    return {"how": "esptool (the board was reset)",
            "mac": match.group(1).lower(),
            "role": chip_line or "an ESP32 with no o1next console",
            "built": None, "slot": None, "pins": None}


def describe(port, esptool, chip):
    """What is behind this port, gentlest probe first."""
    console = ask_console(port)
    if console:
        console["port"] = port
        return console
    rom = ask_esptool(esptool, port, chip)
    if rom:
        rom["port"] = port
        return rom
    return {"port": port, "how": "nothing answered", "mac": None, "role":
            "not an ESP (a USB-TTL adapter is a serial bridge, not a chip esptool "
            "can sync with) or a board held in reset",
            "built": None, "slot": None, "pins": None}


def main():
    want_json = "--json" in sys.argv
    find_mac = None
    if "--find-mac" in sys.argv:
        index = sys.argv.index("--find-mac")
        if index + 1 >= len(sys.argv):
            sys.stderr.write("identify-o1next: --find-mac needs a MAC\n")
            return 2
        find_mac = sys.argv[index + 1].lower().replace("-", ":")
    chip = "esp32s2"
    if "--chip" in sys.argv:
        chip = sys.argv[sys.argv.index("--chip") + 1]

    candidates = ports()
    if not candidates:
        sys.stderr.write("identify-o1next: no serial ports found at all -- is anything "
                         "plugged in?\n")
        return 1

    esptool = find_esptool()
    rows = [describe(port, esptool, chip) for port in candidates]

    if find_mac is not None:
        for row in rows:
            if row.get("mac") == find_mac:
                sys.stdout.write(row["port"] + "\n")
                return 0
        sys.stderr.write("identify-o1next: no board with MAC %s is plugged in\n" % find_mac)
        return 1

    if want_json:
        sys.stdout.write(json.dumps(rows, indent=2) + "\n")
        return 0

    if not esptool:
        sys.stderr.write("identify-o1next: no esptool found, so a bare board cannot be "
                         "asked for its MAC (only a board running o1next firmware can)\n")

    print("%-22s %-18s %-28s %s" % ("PORT", "MAC", "WHAT ANSWERED", "DETAIL"))
    for row in rows:
        detail = row.get("built") or ""
        if row.get("slot"):
            detail = "%s, %s" % (row["slot"], detail)
        if row.get("pins"):
            detail = "%s; %s" % (detail, row["pins"]) if detail else row["pins"]
        print("%-22s %-18s %-28s %s"
              % (row["port"], row.get("mac") or "-", row["how"], detail or row["role"]))
    print()
    print("Write the MAC on each board with a marker: that is the identity, and the")
    print("port name is not -- it changes when a board resets.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
