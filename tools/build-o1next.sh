#!/bin/sh
# Build the O1NEXT WiFi COMM PAC firmware -- and produce ONE image you can flash.
#
# Why a script and not a workflow full of commands: the build a person runs and
# the build CI runs have to be the same build, or CI can pass while the documented
# recipe is wrong.  The workflow calls this script, and so should you.
#
#   tools/build-o1next.sh             # compile, merge, verify
#   tools/build-o1next.sh --clean     # ...without the previous build's objects
#
# Output lands in build/o1next-s2/:
#
#   zimodem-o1next-merged.bin   <- flash THIS at 0x0: bootloader + partition
#                                  table + otadata + application, one file
#   zimodem.ino.bin             the application alone, and the other parts,
#   zimodem.ino.bootloader.bin  for anyone who wants them separately
#   zimodem.ino.partitions.bin
#   zimodem.ino.elf             symbols, for decoding a crash dump
#   SHA256SUMS, BUILD-INFO.txt  what went in, and from where
#
# The merged image is the point of the exercise.  Arduino's own build leaves three
# binaries, plus a fourth that lives in the core (boot_app0.bin), and flashing
# them means knowing four offsets and getting all four right.  esptool's merge_bin
# turns that into one file at offset 0 -- which is what ESPHome and every other
# ESP32 release actually ships.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)

FQBN=${FQBN:-'esp32:esp32:esp32s2:PartitionScheme=huge_app'}
CORE_VERSION=2.0.17
SKETCH="$root/zimodem"
BUILD="$root/build/o1next-s2"

# Fail the build when the sketch passes this share of its application partition.
MAX_APP_PERCENT=${MAX_APP_PERCENT:-90}

clean=no
[ "${1:-}" = "--clean" ] && clean=yes

say() { printf '%s\n' "$*" >&2; }
die() { say "build-o1next.sh: $*"; exit 1; }

command -v arduino-cli >/dev/null 2>&1 || \
    die "no arduino-cli -- see REQUIREMENTS.md in the o1next repository"
command -v python3 >/dev/null 2>&1 || \
    die "no python3 -- the partition table is read back with the core's gen_esp32part.py"

data=$(arduino-cli config get directories.data 2>/dev/null | tr -d '"')
[ -n "$data" ] || die "arduino-cli would not say where its data directory is"

platform="$data/packages/esp32/hardware/esp32/$CORE_VERSION"
[ -d "$platform" ] || die "the esp32 core $CORE_VERSION is not installed --\
 arduino-cli core install esp32:esp32@$CORE_VERSION"

# esptool is a Mach-O binary on macOS and a Python script on Linux, and the core's
# own platform.txt makes the same distinction (tools.esptool_py.cmd vs .cmd.linux)
# -- so this is the same rule rather than an invention.
#
# `-f` as well as `-x`, and that is not pedantry: on Linux the package ships an
# `esptool/` *directory* (the Python package) beside `esptool.py`, and `[ -x ]` is
# true for a directory, so the first version of this picked the directory and
# failed with "Permission denied" on a runner.  Only a regular executable file
# will do.
esptool_bin=""
esptool_py=""
for d in "$data"/packages/esp32/tools/esptool_py/*/; do
    if [ -f "$d/esptool" ] && [ -x "$d/esptool" ]; then esptool_bin="$d/esptool"; break; fi
    if [ -f "$d/esptool.py" ]; then esptool_py="$d/esptool.py"; break; fi
done
if [ -z "$esptool_bin" ] && [ -z "$esptool_py" ]; then
    die "no esptool in the esp32 core -- is the core installation complete?"
fi

esptool() {
    if [ -n "$esptool_bin" ]; then
        "$esptool_bin" "$@"
    else
        python3 "$esptool_py" "$@"
    fi
}

if [ "$clean" = yes ]; then
    rm -rf "$BUILD"
fi
mkdir -p "$BUILD"

# --------------------------------------------------------------- properties --
# Everything the merge needs is READ FROM THE PLATFORM rather than written down
# here: the flash mode, frequency and size, and the bootloader's own offset.  A
# future core that changes them changes this build with them.
say "== reading the flash properties for $FQBN"
props=$(arduino-cli compile --fqbn "$FQBN" --show-properties "$SKETCH" 2>/dev/null)
prop() { printf '%s\n' "$props" | sed -n "s/^$1=//p" | head -1; }

mcu=$(prop 'build\.mcu')
boot_addr=$(prop 'build\.bootloader_addr')
flash_mode=$(prop 'build\.flash_mode')
flash_freq=$(prop 'build\.flash_freq')
flash_size=$(prop 'build\.flash_size')
platform_path=$(prop 'runtime\.platform\.path')

[ -n "$mcu" ]           || die "the platform gave no build.mcu"
[ -n "$boot_addr" ]     || die "the platform gave no build.bootloader_addr"
[ -n "$flash_mode" ]    || die "the platform gave no build.flash_mode"
[ -n "$flash_freq" ]    || die "the platform gave no build.flash_freq"
[ -n "$flash_size" ]    || die "the platform gave no build.flash_size"
[ -n "$platform_path" ] || die "the platform gave no runtime.platform.path"

# The three offsets that are NOT build properties.  They are the constants in the
# core's own upload recipe (tools.esptool_py.upload.pattern_args in platform.txt:
# 0x8000 partitions, 0xe000 boot_app0, 0x10000 application), which is also the
# ESP32 family's standard layout.  They are ASSERTED below rather than trusted.
PART_ADDR=0x8000
OTADATA_ADDR=0xe000
APP_ADDR=0x10000

# ------------------------------------------------------------------ compile --
# The output is captured rather than streamed, because two things in it are
# checked: that the compile succeeded at all, and that the sketch still fits.
say "== compiling"
compile_log="$BUILD/compile.log"
if ! arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" "$SKETCH" \
        > "$compile_log" 2>&1; then
    cat "$compile_log" >&2
    die "the compile failed -- the output above is the whole of it"
fi
cat "$compile_log" >&2

# The application partition is the whole story on this board: there is no OTA
# path, so when the sketch outgrows it the build stops -- and the panel state
# machine (#322), the SSH key store (#321) and the audio are all still to come.
# Failing at 90% with a sentence beats failing at 100% with a linker error.
used=$(sed -n 's/^Sketch uses [0-9]* bytes (\([0-9]*\)%).*/\1/p' "$compile_log" | head -1)
[ -n "$used" ] || die "could not read the sketch size out of the compile output"
if [ "$used" -gt "$MAX_APP_PERCENT" ]; then
    die "the sketch uses ${used}% of its application partition and the budget is ${MAX_APP_PERCENT}%.  See BUILD.md on the partition scheme -- the headroom was the reason for it."
fi
say "   ok   the sketch uses ${used}% of its application partition"

app="$BUILD/zimodem.ino.bin"
boot="$BUILD/zimodem.ino.bootloader.bin"
parts="$BUILD/zimodem.ino.partitions.bin"
otadata="$platform_path/tools/partitions/boot_app0.bin"

for f in "$app" "$boot" "$parts" "$otadata"; do
    [ -f "$f" ] || die "the build did not produce $f"
done

# -------------------------------------------------------------------- merge --
merged="$BUILD/zimodem-o1next-merged.bin"
say "== merging into one image for offset 0x0"
esptool --chip "$mcu" merge_bin -o "$merged" \
    --flash_mode "$flash_mode" --flash_freq "$flash_freq" --flash_size "$flash_size" \
    "$boot_addr" "$boot" \
    "$PART_ADDR" "$parts" \
    "$OTADATA_ADDR" "$otadata" \
    "$APP_ADDR" "$app"

# ------------------------------------------------------------------- verify --
# A wrong offset produces a file that looks fine and does not boot, so nothing
# here is trusted.  The application offset is not a free choice -- the partition
# table declares where the app partition is -- so the table is read back out of
# the build, with the core's own gen_esp32part.py, and the offsets are
# CROSS-CHECKED against it.  Merging the app to 0x20000 "succeeds" and bricks the
# board; this is the check that says so.
say "== verifying the merged image"

table=$(python3 "$platform_path/tools/gen_esp32part.py" "$parts" 2>/dev/null) || \
    die "could not read the partition table back out of $parts"

table_app=$(printf '%s\n' "$table" | awk -F, '$2 == "app" { print $4; exit }')
table_otadata=$(printf '%s\n' "$table" | awk -F, '$1 == "otadata" { print $4; exit }')

[ -n "$table_app" ] || die "the partition table declares no app partition"
[ -n "$table_otadata" ] || die "the partition table declares no otadata partition"

same_hex() { [ "$(printf '%s' "$1" | tr 'A-F' 'a-f')" = "$(printf '%s' "$2" | tr 'A-F' 'a-f')" ]; }

if ! same_hex "$table_app" "$APP_ADDR"; then
    die "the application offset here is $APP_ADDR but the partition table puts the app partition at $table_app -- flashing it elsewhere would not boot"
fi
if ! same_hex "$table_otadata" "$OTADATA_ADDR"; then
    die "the otadata offset here is $OTADATA_ADDR but the partition table puts it at $table_otadata"
fi
say "   ok   the partition table agrees: app $table_app, otadata $table_otadata"

# And that each image really landed where it was put: 0xE9 opens an ESP
# application image (the bootloader is one too) and 0xAA 0x50 opens a partition
# table.  This catches a merge that dropped or misplaced a file.
bytes_at() { od -An -tx1 -j "$2" -N "$3" "$1" | tr -d ' \n'; }
check_magic() {
    got=$(bytes_at "$merged" "$1" "$3")
    if [ "$got" != "$2" ]; then
        die "the merged image has 0x$got at $1, wanted 0x$2 -- the $4 image is not there"
    fi
    say "   ok   $1: 0x$got  ($4)"
}

check_magic "$boot_addr" "e9" 1 "bootloader"
check_magic "$PART_ADDR" "aa50" 2 "partition table"
check_magic "$APP_ADDR" "e9" 1 "application"

# ----------------------------------------------------------------- manifest --
sha_of() {
    if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$@"
    else sha256sum "$@"
    fi
}

say "== checksums and build info"
( cd "$BUILD" && sha_of zimodem-o1next-merged.bin zimodem.ino.bin \
    zimodem.ino.bootloader.bin zimodem.ino.partitions.bin > SHA256SUMS )

{
    echo "O1NEXT WiFi COMM PAC firmware"
    echo "built:           $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "git:             $(cd "$root" && git describe --always --dirty 2>/dev/null || echo 'not a git tree')"
    echo "arduino-cli:     $(arduino-cli version 2>/dev/null | head -1)"
    echo "esp32 core:      $CORE_VERSION"
    echo "fqbn:            $FQBN"
    echo "mcu:             $mcu"
    echo "flash:           mode $flash_mode, freq $flash_freq, size $flash_size"
    echo "bootloader at:   $boot_addr"
    echo "partition table: $PART_ADDR"
    echo "otadata:         $OTADATA_ADDR"
    echo "application at:  $APP_ADDR"
    echo "merged size:     $(wc -c < "$merged" | tr -d ' ') bytes"
    echo
    echo "Flash it with:"
    echo "  esptool --chip $mcu write_flash 0x0 zimodem-o1next-merged.bin"
} > "$BUILD/BUILD-INFO.txt"

say ""
say "== done"
say "   $(basename "$merged")  -- flash at 0x0"
say "   $BUILD/BUILD-INFO.txt, $BUILD/SHA256SUMS"
