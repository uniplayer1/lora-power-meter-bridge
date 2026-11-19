#!/usr/bin/env bash
#
# flash.sh — build + flash the LoRa power-meter firmware (LilyGO T3-S3)
#
# One image fits both boards; the role (basement meter node / flat gateway) is
# chosen once in the setup web UI. See docs/INSTALL.md.
#
# Usage:
#   ./flash.sh                Build, then flash every ESP32-S3 found
#   ./flash.sh 1 | 2          Build, then flash that one board
#   ./flash.sh build          Build only
#   ./flash.sh flash [1|2]    Flash without rebuilding
#   ./flash.sh erase [1|2]    Full chip erase, then flash (wipes all settings)
#   ./flash.sh monitor [1|2]  Serial monitor over USB-CDC
#   ./flash.sh version        Read the firmware stamp off a running board
#   ./flash.sh test [1|2]     Run the on-device test suite over USB
#   ./flash.sh native         Run the host unit tests (no board needed)
#   ./flash.sh clean          Remove build artifacts
#
# Every flash ends by reading the firmware stamp back off the board, so a silent
# failure to boot cannot be mistaken for success: the build writes FW_ID into
# include/firmware_version.h and the firmware prints it at boot and every 10 s.
#
# The image goes into BOTH app slots. After an OTA update the bootloader is
# pointed at app1, and a flash that only wrote app0 would land in the slot the
# board never runs: it would keep the old firmware and the flash would look
# ignored. otadata is cleared as well, so the boot pointer is never a question.
#
# The T3-S3 uses USB-Serial/JTAG (not a USB-UART bridge), so DTR/RTS toggling
# does nothing and --after hard-reset is a no-op: a board flashed that way
# stays in the ROM downloader with no serial output and no WiFi. This script
# always uses --after watchdog-reset, which really restarts the chip into the
# application. If a board is stuck in a boot loop, hold BOOT while it runs.
#
# Serial output requires the firmware to be built with USB-CDC — those flags
# live in boards/lilygo_t3s3.json. Without them Serial stays on UART0 and the
# monitor shows nothing over USB.
#
# Port auto-detection: the script discovers ESP32-S3 devices on /dev/ttyACM*
# and orders them by MAC address (board 1 = lowest MAC). If only one board is
# connected it defaults to board 1.
#

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
FIRMWARE_DIR="$ROOT_DIR/firmware"
ENV="t3s3"
PART_CSV="$FIRMWARE_DIR/partitions/lilygo_4mb_ota.csv"
MAX_RETRIES=3
RETRY_DELAY=2

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

log()  { echo -e "${CYAN}[FLASH]${NC} $*"; }
ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
err()  { echo -e "${RED}[ERR]${NC} $*" >&2; }

# ── Port auto-detection ───────────────────────────────────────
# Discover ESP32-S3 boards and return their ports in MAC order.
#
# Probing uses --before default-reset, not no-reset: a board whose application
# owns the USB CDC does not answer esptool otherwise. The price is that probing
# leaves the board in the ROM downloader, which is exactly where flashing wants
# it anyway.
discover_ports() {
    local found=()
    local acm mac
    for acm in /dev/ttyACM*; do
        [ -e "$acm" ] || continue
        mac=$(esptool --chip esp32s3 --port "$acm" --before default-reset read-mac 2>/dev/null \
              | awk '/MAC:/ {print $2; exit}')
        [ -n "$mac" ] || continue
        found+=("$mac $acm")
    done
    [ "${#found[@]}" -eq 0 ] && return 0
    printf '%s\n' "${found[@]}" | sort | awk '{print $2}'
}

# Find the port a board is printing on. Needed for monitor/test, and after
# flashing: once the application takes over USB the board re-enumerates and the
# ttyACM number changes.
running_port() {
    local dev_num="$1"
    local ports count idx
    ports=$(python3 "$ROOT_DIR/tools/serial_find.py" --timeout 12 2>/dev/null | sort || true)
    count=$(printf '%s\n' "$ports" | grep -c . || true)
    if [ "$count" -eq 0 ]; then
        echo "/dev/ttyACM0"
        return 0
    fi
    idx=$((dev_num - 1))
    [ "$idx" -ge "$count" ] && idx=0
    printf '%s\n' "$ports" | sed -n "$((idx + 1))p"
}

note_app_port() {
    local found
    found=$(python3 "$ROOT_DIR/tools/serial_find.py" --timeout 12 2>/dev/null | paste -sd' ' - || true)
    [ -n "$found" ] && log "Application serial output on: $found"
    return 0
}

# The stamp the current build carries (written by tools/gen_version.py).
built_firmware_id() {
    [ -f "$FIRMWARE_DIR/include/firmware_version.h" ] || return 0
    sed -n 's/^#define FW_ID "\(.*\)"$/\1/p' "$FIRMWARE_DIR/include/firmware_version.h"
}

# Reads the firmware stamp back off the board. Because the app re-enumerates on
# a new ttyACM number after every reset, "which firmware is running" cannot be
# answered by the port alone - the board has to say it itself.
confirm_running_firmware() {
    local dev_num="$1"
    local expected
    expected=$(built_firmware_id)
    if [ -z "$expected" ]; then
        warn "No firmware stamp in the build - skipping the version check"
        note_app_port
        return 0
    fi

    log "Waiting for board $dev_num to report $expected ..."
    local line
    line=$(python3 "$ROOT_DIR/tools/serial_find.py" --expect "$expected" --match-only --timeout 12 2>/dev/null | head -1)
    if [ -n "$line" ]; then
        ok "Board $dev_num runs $expected"
        log "  $line"
    else
        warn "Board $dev_num never reported $expected over USB"
        warn "It may be stuck in the ROM downloader or boot-looping - check the OLED, then: ./flash.sh version"
        note_app_port
    fi
    return 0
}

# Get the port for a specific board number, stable across replugs.
get_port() {
    local dev_num="$1"
    local hint_port=""
    case "$dev_num" in
        1) hint_port="/dev/ttyACM0" ;;
        2) hint_port="/dev/ttyACM1" ;;
    esac

    # esptool prints the MAC twice (once on connect, once after the reset), so
    # discover_ports filters to the first hit; drop any leftover blank lines so
    # the index arithmetic below cannot be shifted by an empty entry.
    local all_ports count
    all_ports=$(discover_ports | grep . || true)
    count=$(printf '%s\n' "$all_ports" | grep -c . || true)

    if [ "$count" -eq 0 ]; then
        # Nothing identified as an ESP32-S3; fall back to the hint if present.
        if [ -e "$hint_port" ]; then
            echo "$hint_port"
            return 0
        fi
        err "No ESP32-S3 boards found on /dev/ttyACM*"
        return 1
    fi

    local idx=$((dev_num - 1))
    if [ "$idx" -ge "$count" ]; then
        err "Board $dev_num not found (only $count connected)"
        printf '%s\n' "$all_ports" | while read -r p; do err "  $p"; done
        return 1
    fi

    printf '%s\n' "$all_ports" | sed -n "$((idx + 1))p"
    return 0
}

check_port() {
    local port="$1" dev_num="$2"
    if [ ! -e "$port" ]; then
        err "Board $dev_num not found at $port"
        err "Make sure it is connected via USB"
        return 1
    fi
    if [ ! -w "$port" ]; then
        warn "No write permission on $port — install the shipped rule once:"
        warn "  sudo install -m644 udev/99-lora-power.rules /etc/udev/rules.d/"
        warn "  sudo udevadm control --reload && sudo udevadm trigger --subsystem-match=tty"
    fi
    return 0
}

# ── Build ─────────────────────────────────────────────────────
build_firmware() {
    log "Building $ENV..."
    if ! (cd "$FIRMWARE_DIR" && pio run -e "$ENV"); then
        err "$ENV build FAILED"
        return 1
    fi
    ok "$ENV built successfully"
    return 0
}

# ── Flash ─────────────────────────────────────────────────────
# The app offset comes from the partition table, not a constant: the OTA
# layout puts app0 at 0x11000, and hardcoding 0x10000 would flash an image
# linked for the wrong address.
app_offset() {
    awk -F, '/^[[:space:]]*app0[[:space:]]*,/ {gsub(/[[:space:]]/, "", $4); print $4}' "$PART_CSV"
}

app1_offset() {
    awk -F, '/^[[:space:]]*app1[[:space:]]*,/ {gsub(/[[:space:]]/, "", $4); print $4}' "$PART_CSV"
}

# The OTA boot pointer. A USB flash always writes app0 - but once a board has
# taken an OTA update, the bootloader is pointed at app1, and the freshly
# flashed app0 is never booted: the board keeps running the old firmware and the
# flash looks like it was ignored (it was, from the board's point of view).
# Clearing otadata hands the boot back to app0. NVS - every setting - is
# untouched, only the two-slot choice is reset.
otadata_region() {
    awk -F, '/^[[:space:]]*otadata[[:space:]]*,/ {gsub(/[[:space:]]/, "", $4); gsub(/[[:space:]]/, "", $5); print $4, $5}' "$PART_CSV"
}

clear_otadata() {
    local port="$1" region="$2"
    [ -n "$region" ] || return 0
    log "Clearing otadata ($region) so the board boots the image just written ..."
    # shellcheck disable=SC2086
    esptool --chip esp32s3 --port "$port" --before default-reset --after watchdog-reset \
        erase-region $region 2>&1 | tail -2
    return 0
}

flash_env() {
    local port="$1" dev_num="$2"
    local builddir="$FIRMWARE_DIR/.pio/build/$ENV"
    local bootloader="$builddir/bootloader.bin"
    local partitions="$builddir/partitions.bin"
    local firmware="$builddir/firmware.bin"
    local app_off app1_off ota_region

    for f in "$bootloader" "$partitions" "$firmware"; do
        if [ ! -f "$f" ]; then
            err "Missing build output: $f"
            err "Run './flash.sh build' first, or './flash.sh $dev_num' to build+flash"
            return 1
        fi
    done

    app_off=$(app_offset)
    if [ -z "$app_off" ]; then
        err "Could not read app0 offset from $PART_CSV"
        return 1
    fi
    ota_region=$(otadata_region)

    # Write the app into BOTH slots. A board that has ever taken an OTA update
    # boots the slot this script used not to touch, so a flash that only wrote
    # app0 landed in the slot the board never runs: it kept the old firmware and
    # the flash looked ignored. Writing both makes the outcome independent of
    # whatever the boot pointer happens to say.
    app1_off=$(app1_offset)
    local extra=()
    [ -n "$app1_off" ] && extra=("$app1_off" "$firmware")

    log "Flashing $ENV → $port (app @ $app_off) ..."
    check_port "$port" "$dev_num" || return 1

    local attempt=1
    while [ "$attempt" -le "$MAX_RETRIES" ]; do
        [ "$attempt" -gt 1 ] && { log "Retry $attempt/$MAX_RETRIES (waiting ${RETRY_DELAY}s)..."; sleep "$RETRY_DELAY"; }

        # --after watchdog-reset is mandatory on this board. --after hard-reset
        # only toggles RTS, which the ESP32-S3 USB-Serial/JTAG ignores: the chip
        # would silently stay in the ROM downloader and never run the firmware
        # (no serial output, no WiFi). The watchdog reset is issued through the
        # stub and actually restarts the chip into the application.
        log "Connecting with default-reset..."
        if esptool --chip esp32s3 --port "$port" --baud 921600 \
            --before default-reset --after watchdog-reset \
            write-flash 0x0 "$bootloader" 0x8000 "$partitions" "$app_off" "$firmware" "${extra[@]}" 2>&1; then
            ok "Flashed to $port"
            clear_otadata "$port" "$ota_region"
            confirm_running_firmware "$dev_num"
            return 0
        fi

        log "Retrying with no-reset..."
        if esptool --chip esp32s3 --port "$port" --baud 921600 \
            --before no-reset --after watchdog-reset \
            write-flash 0x0 "$bootloader" 0x8000 "$partitions" "$app_off" "$firmware" "${extra[@]}" 2>&1; then
            ok "Flashed to $port"
            clear_otadata "$port" "$ota_region"
            confirm_running_firmware "$dev_num"
            return 0
        fi

        attempt=$((attempt + 1))
    done

    err "Failed to flash board $dev_num after $MAX_RETRIES attempts"
    err ""
    err "Manual recovery:"
    err "  1. Hold BOOT on the board, then re-run: ./flash.sh flash $dev_num"
    err "  2. Or press RESET while holding BOOT, then release both"
    err "  3. If it is in a boot loop it still accepts connections between reboots"
    return 1
}

erase_and_flash() {
    local port="$1" dev_num="$2"
    warn "Full chip erase on $port wipes WiFi credentials and all settings"
    if ! esptool --chip esp32s3 --port "$port" --before default-reset --after watchdog-reset erase-flash; then
        err "erase-flash failed on $port"
        return 1
    fi
    ok "Chip erased"
    # An erase resets the USB device: the board re-enumerates under a different
    # ttyACM number, so the port is resolved again before flashing.
    sleep 2
    port=$(get_port "$dev_num") || return 1
    flash_env "$port" "$dev_num"
}

monitor_device() {
    local port="$1" dev_num="$2"
    if [ ! -e "$port" ]; then
        err "Board $dev_num not found at $port"
        return 1
    fi
    log "Serial monitor on $port (115200 baud, Ctrl+] to exit)..."
    (cd "$FIRMWARE_DIR" && pio device monitor -p "$port" -b 115200)
}

run_device_tests() {
    local port="$1" dev_num="$2"
    log "Running on-device tests on $port ..."
    (cd "$FIRMWARE_DIR" && pio test -e "$ENV" -f test_device --upload-port "$port")
}

run_native_tests() {
    log "Running host unit tests (env:native) ..."
    (cd "$FIRMWARE_DIR" && pio test -e native)
}

clean_build() {
    log "Cleaning build artifacts..."
    (cd "$FIRMWARE_DIR" && pio run -e "$ENV" -t clean)
    ok "Clean complete"
}

# ── Main ──────────────────────────────────────────────────────

FAILED=0

case "${1:-all}" in
    build)
        build_firmware || FAILED=1
        ;;
    clean)
        clean_build
        ;;
    native)
        run_native_tests || FAILED=1
        ;;
    version)
        stamp=$(built_firmware_id)
        [ -n "$stamp" ] && log "Built firmware: $stamp"
        log "Listening for a running board (Ctrl-C to stop) ..."
        out=$(python3 "$ROOT_DIR/tools/serial_find.py" --expect "FW:" --match-only --timeout 12 2>/dev/null || true)
        if [ -n "$out" ]; then
            printf '%s\n' "$out"
        else
            err "No board reported a firmware stamp on any /dev/ttyACM*"
            FAILED=1
        fi
        ;;
    test)
        dev="${2:-1}"
        port="${2:-}"
        case "$port" in /dev/*) ;; *) port=$(running_port "$dev") ;; esac
        run_device_tests "$port" "$dev" || FAILED=1
        ;;
    monitor)
        dev="${2:-1}"
        port="${2:-}"
        case "$port" in /dev/*) ;; *) port=$(running_port "$dev") ;; esac
        monitor_device "$port" "$dev"
        ;;
    erase)
        dev="${2:-1}"
        port=$(get_port "$dev") || { FAILED=1; port=""; }
        [ -n "$port" ] && { erase_and_flash "$port" "$dev" || FAILED=1; }
        ;;
    flash)
        dev="${2:-}"
        if [ -z "$dev" ]; then
            for i in 0 1; do
                n=$((i + 1))
                port=$(get_port "$n") || { FAILED=1; continue; }
                flash_env "$port" "$n" || FAILED=1
            done
        else
            port=$(get_port "$dev") || { FAILED=1; port=""; }
            [ -n "$port" ] && { flash_env "$port" "$dev" || FAILED=1; }
        fi
        ;;
    1|2)
        dev="$1"
        port=$(get_port "$dev") || { FAILED=1; port=""; }
        if [ -n "$port" ]; then
            build_firmware && flash_env "$port" "$dev" || FAILED=1
        fi
        ;;
    all|*)
        build_firmware || FAILED=1
        if [ "$FAILED" -eq 0 ]; then
            for i in 0 1; do
                n=$((i + 1))
                port=$(get_port "$n") || { FAILED=1; continue; }
                flash_env "$port" "$n" || FAILED=1
            done
        fi
        ;;
esac

if [ "$FAILED" -ne 0 ]; then
    err "One or more operations failed"
    exit 1
fi

echo
ok "${BOLD}Done.${NC} Each board now runs the same image — pick the role in the setup page:"
echo "  1. Join the board's WiFi AP (SSID LilyGo-… / password shown on the OLED)"
echo "  2. Open http://192.168.4.1 and choose Basement meter node or Flat gateway"
echo "  3. Full walkthrough: docs/INSTALL.md"
exit 0
