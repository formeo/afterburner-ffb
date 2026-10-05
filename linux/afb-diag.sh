#!/usr/bin/env bash
# afb-diag.sh — diagnostics for Thrustmaster Top Gun Afterburner FFB on Linux.
# Usage: sudo bash afb-diag.sh [VID] [PID]      (default 044f b550)
# Collects descriptors, HID report descriptor, USB traffic captures,
# and tries to bind the kernel iforce driver. Everything is restored on exit.
set -u

VID=${1:-044f}; PID=${2:-b550}
[ "$EUID" -eq 0 ] || exec sudo bash "$0" "$@"

USER_HOME=$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)
OUT="$USER_HOME/afb-diag-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
SUM="$OUT/summary.txt"
say()  { echo -e "\n>>> $*"; }
DRV=""; IFACES=(); CAP=""
finish() {
  [ -n "$CAP" ] && kill -INT "$CAP" 2>/dev/null && wait "$CAP" 2>/dev/null
  if [ -n "$DRV" ]; then
    echo "$VID $PID" > "/sys/bus/usb/drivers/$DRV/remove_id" 2>/dev/null
    for f in "${IFACES[@]}"; do
      [ -e "/sys/bus/usb/drivers/$DRV/$f" ] && echo -n "$f" > "/sys/bus/usb/drivers/$DRV/unbind"
      [ -e "/sys/bus/usb/drivers/usbhid/$f" ] || echo -n "$f" > /sys/bus/usb/drivers/usbhid/bind 2>/dev/null
    done
    echo "Stock driver restored."
  fi
  tar czf "$OUT.tar.gz" -C "$(dirname "$OUT")" "$(basename "$OUT")"
  chown -R "${SUDO_USER:-root}": "$OUT" "$OUT.tar.gz"
  echo -e "\nSummary: $SUM\nArchive: $OUT.tar.gz"
}
trap finish EXIT
note() { echo "$*" | tee -a "$SUM"; }

# ---------- tools ----------
say "Installing tools (evtest, joystick, tshark, usbutils)..."
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq evtest joystick tshark usbutils >/dev/null 2>&1 \
  || echo "WARN: apt install failed, continuing with what is installed"

# ---------- find device ----------
DEV=""
for d in /sys/bus/usb/devices/*; do
  [ -f "$d/idVendor" ] || continue
  if [ "$(cat "$d/idVendor")" = "$VID" ] && [ "$(cat "$d/idProduct")" = "$PID" ]; then
    DEV=$(basename "$d")
  fi
done
if [ -z "$DEV" ]; then
  echo "Device $VID:$PID not found. All USB devices:"
  lsusb | tee "$OUT/lsusb.txt"
  echo "Re-run with the right IDs: sudo bash $0 <vid> <pid>"
  exit 1
fi
SYS=/sys/bus/usb/devices/$DEV
BUS=$(cat "$SYS/busnum"); DEVNUM=$(cat "$SYS/devnum")

note "=== afb-diag $(date -Is) ==="
note "kernel: $(uname -r)"
note "device: $VID:$PID at $DEV (bus $BUS, addr $DEVNUM)"
note "product: $(cat "$SYS/product" 2>/dev/null) / $(cat "$SYS/manufacturer" 2>/dev/null)"
note "bcdDevice: $(cat "$SYS/bcdDevice") speed: $(cat "$SYS/speed") Mbit/s"

lsusb -v -d "$VID:$PID" > "$OUT/lsusb-v.txt" 2>&1
lsusb -t > "$OUT/lsusb-t.txt" 2>&1
dmesg > "$OUT/dmesg-initial.txt"

# ---------- interfaces & endpoints ----------
note ""
note "--- interfaces ---"
for i in "$SYS"/"$DEV":*; do
  [ -d "$i" ] || continue
  name=$(basename "$i"); IFACES+=("$name")
  drv=$(basename "$(readlink "$i/driver" 2>/dev/null)" 2>/dev/null)
  note "$name class=$(cat "$i/bInterfaceClass") sub=$(cat "$i/bInterfaceSubClass") proto=$(cat "$i/bInterfaceProtocol") eps=$(cat "$i/bNumEndpoints") driver=${drv:-none}"
  for ep in "$i"/ep_*; do
    [ -d "$ep" ] || continue
    note "   $(basename "$ep") addr=$(cat "$ep/bEndpointAddress") $(cat "$ep/type") $(cat "$ep/direction") interval=$(cat "$ep/interval") maxp=$(cat "$ep/wMaxPacketSize")"
  done
done

# ---------- HID report descriptor (native, from the device) ----------
note ""
note "--- native HID report descriptor ---"
HIDDEV=$(ls -d /sys/bus/hid/devices/*:"${VID^^}":"${PID^^}".* 2>/dev/null | head -1)
if [ -n "$HIDDEV" ]; then
  od -An -tx1 -v "$HIDDEV/report_descriptor" > "$OUT/hid-report-descriptor.txt"
  note "$(basename "$HIDDEV"): $(stat -c %s "$HIDDEV/report_descriptor") bytes (full dump in hid-report-descriptor.txt)"
  head -3 "$OUT/hid-report-descriptor.txt" | tee -a "$SUM"
else
  note "no HID device bound (interface is not HID or HID parse failed — see dmesg-initial.txt)"
fi
grep -A9 -i "Vendor=$VID Product=$PID" /proc/bus/input/devices > "$OUT/input-devices-usbhid.txt"

# ---------- capture helpers ----------
cap_start() { tshark -i "usbmon$BUS" -w "$OUT/$1.pcapng" -q >/dev/null 2>&1 & CAP=$!; sleep 2; }
cap_stop()  { [ -n "$CAP" ] && kill -INT "$CAP" 2>/dev/null && wait "$CAP" 2>/dev/null; CAP=""; }
modprobe usbmon

event_of() { ls -d "$1"/input/input*/event* 2>/dev/null | head -1 | xargs -r basename; }

# ---------- phase 1: input under the stock HID driver ----------
EV=""
[ -n "$HIDDEV" ] && EV=$(event_of "$HIDDEV")
if [ -n "$EV" ]; then
  say "PHASE 1: stock driver. For 20 s: move X, Y, throttle, rudder to the stops, press every button, roll the hat."
  read -rp "Press Enter to start..."
  cap_start usbhid-input
  timeout 20 evtest "/dev/input/$EV" > "$OUT/evtest-usbhid.txt" 2>&1
  cap_stop
  note ""
  note "--- phase 1: /dev/input/$EV, $(grep -c '^Event:' "$OUT/evtest-usbhid.txt") events recorded"
  grep -E "Input device name|Event code" "$OUT/evtest-usbhid.txt" | head -40 | tee -a "$SUM"
else
  note "phase 1 skipped: no input device from the stock driver"
fi

# ---------- phase 2: kernel iforce driver ----------
say "PHASE 2: try the kernel I-Force driver."
echo "WARNING: the stick may move by itself once force feedback is enabled."
echo "Hold it lightly, keep fingers clear, be ready to pull the USB cable."
read -rp "Continue? [y/N] " ok
[ "${ok,,}" = "y" ] || { note "phase 2 skipped by user"; exit 0; }

modprobe iforce_usb 2>>"$SUM" || note "modprobe iforce_usb failed"
IFDRV=$(ls /sys/bus/usb/drivers | grep -i iforce | head -1)
note ""
note "--- phase 2: iforce driver in sysfs: ${IFDRV:-NOT FOUND}"
[ -n "$IFDRV" ] || exit 1
DRV=$IFDRV   # from here on, finish() will restore the stock driver


NLOG=$(dmesg | wc -l)
cap_start iforce-probe
for f in "${IFACES[@]}"; do
  [ -e "/sys/bus/usb/drivers/usbhid/$f" ] && echo -n "$f" > /sys/bus/usb/drivers/usbhid/unbind
done
echo "$VID $PID" > "/sys/bus/usb/drivers/$DRV/new_id" 2>/dev/null
sleep 1
for f in "${IFACES[@]}"; do
  [ -e "/sys/bus/usb/drivers/$DRV/$f" ] || echo -n "$f" > "/sys/bus/usb/drivers/$DRV/bind" 2>>"$SUM"
done
sleep 7   # the driver waits up to 5 s for the device's first reply
cap_stop
dmesg | tail -n +$((NLOG + 1)) > "$OUT/dmesg-iforce.txt"
note "dmesg after bind:"
tee -a "$SUM" < "$OUT/dmesg-iforce.txt"

BOUND=""
for f in "${IFACES[@]}"; do [ -e "/sys/bus/usb/drivers/$DRV/$f" ] && BOUND=$f; done
if [ -z "$BOUND" ]; then
  note "RESULT: iforce did NOT bind (probe rejected the interface or the device did not answer)"
  exit 0
fi
EV=$(event_of "/sys/bus/usb/devices/$BOUND")
note "RESULT: iforce bound to $BOUND, input node: ${EV:-none}"
grep -A9 -i "Vendor=$VID Product=$PID" /proc/bus/input/devices > "$OUT/input-devices-iforce.txt"
[ -n "$EV" ] || exit 0

say "Input via iforce: 15 s, move all axes and press buttons."
read -rp "Press Enter to start..."
cap_start iforce-input
timeout 15 evtest "/dev/input/$EV" > "$OUT/evtest-iforce.txt" 2>&1
cap_stop
note "iforce input: $(grep -c '^Event:' "$OUT/evtest-iforce.txt") events"
grep -E "Input device name|Event code|Event type 21" "$OUT/evtest-iforce.txt" | head -40 | tee -a "$SUM"

say "Force feedback test with fftest. Type an effect number + Enter, -1 stops. Ctrl+C to finish."
read -rp "Press Enter to start..."
cap_start iforce-fftest
fftest "/dev/input/$EV" 2>&1 | tee "$OUT/fftest.txt"
cap_stop
note "fftest log saved (fftest.txt)"
