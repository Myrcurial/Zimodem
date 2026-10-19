#!/bin/sh
# Shared environment for the O1NEXT build and flash scripts.  SOURCED, not run.
#
# Both scripts need the same three things -- where arduino-cli keeps its data,
# how to invoke esptool on this host, and the two output helpers -- and the point
# of factoring them out is that the *discovery* exists once.  The esptool rule is
# the interesting one, and it is not obvious.

O1NEXT_CORE_VERSION=${O1NEXT_CORE_VERSION:-2.0.17}

say() { printf '%s\n' "$*" >&2; }
die() { say "$(basename "$0"): $*"; exit 1; }

command -v arduino-cli >/dev/null 2>&1 || \
    die "no arduino-cli -- see REQUIREMENTS.md in the o1next repository"
command -v python3 >/dev/null 2>&1 || \
    die "no python3 -- the partition table is read back with the core's gen_esp32part.py"

DATA=$(arduino-cli config get directories.data 2>/dev/null | tr -d '"')
[ -n "$DATA" ] || die "arduino-cli would not say where its data directory is"

PLATFORM="$DATA/packages/esp32/hardware/esp32/$O1NEXT_CORE_VERSION"
[ -d "$PLATFORM" ] || die "the esp32 core $O1NEXT_CORE_VERSION is not installed --\
 arduino-cli core install esp32:esp32@$O1NEXT_CORE_VERSION"

# esptool is a Mach-O binary on macOS and a Python script on Linux, and the core's
# own platform.txt makes the same distinction (tools.esptool_py.cmd vs
# .cmd.linux) -- so this is the same rule rather than an invention.
#
# `-f` as well as `-x`, and that is not pedantry: on Linux the package ships an
# `esptool/` *directory* (the Python package) beside `esptool.py`, and `[ -x ]` is
# true for a directory, so an earlier version of this picked the directory and
# failed with "Permission denied" on a runner.  Only a regular executable file
# will do.
O1NEXT_ESPTOOL_BIN=""
O1NEXT_ESPTOOL_PY=""
for d in "$DATA"/packages/esp32/tools/esptool_py/*/; do
    if [ -f "$d/esptool" ] && [ -x "$d/esptool" ]; then O1NEXT_ESPTOOL_BIN="$d/esptool"; break; fi
    if [ -f "$d/esptool.py" ]; then O1NEXT_ESPTOOL_PY="$d/esptool.py"; break; fi
done
if [ -z "$O1NEXT_ESPTOOL_BIN" ] && [ -z "$O1NEXT_ESPTOOL_PY" ]; then
    die "no esptool in the esp32 core -- is the core installation complete?"
fi

esptool() {
    if [ -n "$O1NEXT_ESPTOOL_BIN" ]; then
        "$O1NEXT_ESPTOOL_BIN" "$@"
    else
        python3 "$O1NEXT_ESPTOOL_PY" "$@"
    fi
}
