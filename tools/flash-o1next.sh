#!/bin/sh
# Flash the O1NEXT firmware and start it -- without touching the board.
#
#   tools/flash-o1next.sh                # flash the build, then reset into it
#   tools/flash-o1next.sh --console 20   # ...and then watch the console for 20s
#   tools/flash-o1next.sh --port /dev/cu.usbmodem01
#   tools/flash-o1next.sh --image other.bin
#
# WHY THIS IS A SCRIPT AND NOT ONE LINE OF DOCUMENTATION
#
# The only programming port on this board is the ESP32-S2's own USB, and the ROM
# cannot reset the chip over it.  esptool says so itself:
#
#   "esptool.py can not exit the download mode over USB.  To run the app, reset
#    the chip manually."
#
# ...which is a button press, on a device whose USB port is documented as being
# for programming.  It does not have to be.  RTC_CNTL_OPTIONS0 bit 31 is
# SW_SYS_RST -- a write-only system reset, and exactly what esp_restart() does
# inside the firmware.  One write_mem from here and the image we just flashed is
# running.
#
# So the button keeps one job: entering download mode.  That is needed only when
# the firmware is not running -- the very first flash, or a bricked board -- and
# everything after that goes over OTA on the console.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
# shellcheck source=o1next-env.sh
. "$here/o1next-env.sh"

CHIP=${CHIP:-esp32s2}
IMAGE=${IMAGE:-$root/build/o1next-s2/zimodem-o1next-merged.bin}
PORT=${PORT:-}
CONSOLE_SECONDS=0

# The RTC_CNTL SW_SYS_RST address and bit are recorded in the flash section below,
# where the attempt to use them is documented and why it did not work.

while [ $# -gt 0 ]; do
    case "$1" in
        --port)    PORT=${2:?--port needs a value}; shift 2 ;;
        --image)   IMAGE=${2:?--image needs a value}; shift 2 ;;
        --console) CONSOLE_SECONDS=${2:?--console needs seconds}; shift 2 ;;
        *) die "unknown argument: $1" ;;
    esac
done

[ -f "$IMAGE" ] || die "no image at $IMAGE -- build it first: tools/build-o1next.sh"

# The board re-enumerates with a different name depending on whether it is in
# download mode or running, and a stale node can linger, so this takes the newest
# rather than the first.  --port overrides it when that guesses wrong.
find_port() { ls -t /dev/cu.usbmodem* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1; }

if [ -z "$PORT" ]; then
    PORT=$(find_port)
    [ -n "$PORT" ] || die "no serial port -- is the board connected, and in download mode?"
fi
say "== port  $PORT"
say "== image $IMAGE ($(wc -c < "$IMAGE" | tr -d ' ') bytes)"

# -------------------------------------------------------------------- flash --
# `-e` erases the whole chip first.  The partition table changes often enough
# between builds (huge_app, min_spiffs, ...) that a stale one is a real way to
# lose an afternoon, and this is the only writer.
#
# THE RESET IS ESPTOOL'S OWN, so do NOT add `--after no_reset` here.  That was
# the first version of this script, on the theory that the ROM cannot reset the
# chip over USB -- which is what esptool itself warns:
#
#   "esptool.py can not exit the download mode over USB.  To run the app, reset
#    the chip manually."
#
# ...so the reset was going to be a write_mem to RTC_CNTL_OPTIONS0's SW_SYS_RST
# bit (0x3f408000, bit 31 -- the same thing esp_restart() does).  **It does not
# work on this board, and the reason is worth keeping.**  The write lands and the
# port drops, so it looks like a reset -- but the chip comes back up in download
# mode, every time.  GPIO0 reads high, so it is not the strap: the board's
# auto-reset circuit holds GPIO0 low while the port is open, and only esptool's
# own sequence releases it before pulsing EN.  esptool's default hard reset does
# the whole job, and the warning above is a generic USB-CDC one that does not
# apply here.
#
# A board with no auto-reset circuit at all would be the case for the SW_SYS_RST
# trick.  Our own PCB is one -- see BUILD.md.
say "== flashing, and resetting into the application"
# esptool's own reset is used (see the note above), and that means the port can
# re-enumerate underneath it: resetting *running* firmware into the ROM's download
# mode changes the USB device, and the open handle dies with it.  That is not a
# failure, it is the reset doing its job -- so the flash is attempted more than
# once, and the second attempt finds the chip already in download mode and gets on
# with it.  Measured: this is what a first attempt on a running board looks like.
attempt=1
while : ; do
    if [ "$attempt" -gt 1 ]; then
        say "   (attempt $attempt -- the port moved, which is the reset working)"
    fi
    if esptool --chip "$CHIP" --port "$PORT" write_flash -e 0x0 "$IMAGE"; then
        break
    fi
    if [ "$attempt" -ge 3 ]; then
        die "the flash failed three times -- is the board in download mode, and is nothing else holding $PORT?"
    fi
    attempt=$((attempt + 1))
    sleep 1
    PORT=$(find_port)
    [ -n "$PORT" ] || die "the port disappeared -- is the board still connected?"
done

# ------------------------------------------------------------------ console --
if [ "$CONSOLE_SECONDS" -gt 0 ]; then
    # The port disappears during the reset and comes back, so this waits for it
    # rather than guessing a delay: the firmware is waiting for *us* at the other
    # end of this race, for O1NEXT_USB_WAIT_MS, and opening as soon as the port
    # appears is what stops the boot banner going into the void.
    say "== waiting for the console"
    PORT=""
    i=0
    while [ "$i" -lt 150 ]; do
        PORT=$(find_port)
        if [ -n "$PORT" ]; then
            break
        fi
        sleep 0.1
        i=$((i + 1))
    done
    [ -n "$PORT" ] || die "the console did not come back -- is CDCOnBoot=cdc in the build?"
    # A pattern, not the path: the re-enumerated device can land on a different
    # name, and the reader re-resolves on every reopen because of it.
    console_pattern=$(printf '%s' "$PORT" | sed 's/[0-9]*$/*/')
    say "== console on $console_pattern for ${CONSOLE_SECONDS}s"
    python3 "$here/read-console.py" "$console_pattern" "$CONSOLE_SECONDS"
fi

say ""
say "== done"
