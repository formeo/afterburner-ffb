#!/usr/bin/env python3
"""afb_hid.py - talk to Thrustmaster Top Gun Afterburner FFB (044f:b550) via Linux hidraw.

All report IDs and field layouts come from the device's own 851-byte HID report
descriptor (dumped by afb-diag.sh). Two places are ambiguous and are exposed as
--variant: the opcode values for Device Control (0x52) and Effect Operation (0x51).
  desc  - values as the HID descriptor declares them (1-based array indices)
  linux - values the Linux iforce driver uses for the same I-Force commands

Usage (run as root or add a udev rule for /dev/hidraw*):
  sudo python3 afb_hid.py info
  sudo python3 afb_hid.py watch
  sudo python3 afb_hid.py stop
  sudo python3 afb_hid.py direct --x 15
  sudo python3 afb_hid.py const --variant desc --mag 30
  sudo python3 afb_hid.py hold --time 4
  sudo python3 afb_hid.py vendor40 --index 3 --value 0
  sudo python3 afb_hid.py sweep
  sudo python3 afb_hid.py live --time 6
  sudo python3 afb_hid.py condition --kind spring --time 5
  sudo python3 afb_hid.py periodic --wave sine --period 200 --time 2
"""
import argparse
import fcntl
import glob
import os
import select
import struct
import sys
import time

VID, PID = 0x044F, 0xB550
MAX_SAFE_MAG = 80   # of 127; raise only with --i-know
MAX_SAFE_GAIN = 64  # of 128

# Opcodes that differ between the descriptor and the Linux iforce driver.
VARIANTS = {
    #          enable  stop_all  start  stop
    "desc":  dict(enable=3, stop_all=2, start=2, stop=1),
    "linux": dict(enable=4, stop_all=None, start=1, stop=0),
}


def find_hidraw():
    for p in glob.glob("/sys/class/hidraw/hidraw*/device/uevent"):
        with open(p) as f:
            for line in f:
                if line.startswith("HID_ID="):
                    _, v, d = line.strip().split("=")[1].split(":")
                    if int(v, 16) == VID and int(d, 16) == PID:
                        return "/dev/" + p.split("/")[4]
    sys.exit("Joystick not found. Is it plugged in and bound to usbhid (not iforce)?")


def hidiocgfeature(length):
    return (3 << 30) | (length << 16) | (ord("H") << 8) | 0x07


class Dev:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR)
        self.flags = None      # last status flags byte
        self.playing = set()   # blocks the device reports as playing
        self.t0 = time.time()

    def send(self, *data, quiet=False):
        buf = bytes(b & 0xFF for b in data)
        if not quiet:
            print(f"  OUT    +{time.time() - self.t0:5.2f}s", buf.hex(" "))
        os.write(self.fd, buf)

    def feature(self, rid, length):
        buf = bytearray([rid]) + bytearray(length)
        fcntl.ioctl(self.fd, hidiocgfeature(len(buf)), buf)
        return bytes(buf)

    def read(self, timeout=0.05):
        r, _, _ = select.select([self.fd], [], [], timeout)
        return os.read(self.fd, 64) if r else None

    def pump(self, seconds):
        """Read reports for `seconds`. Status reports (0x02) cycle through blocks 0-19,
        so print only when the flags change or a block starts/stops playing."""
        end = time.time() + seconds
        while time.time() < end:
            r = self.read(0.02)
            if not r or r[0] != 0x02:
                continue
            blk, playing = r[2] & 0x7F, r[2] >> 7
            was_playing = blk in self.playing
            if r[1] != self.flags or bool(playing) != was_playing:
                self.flags = r[1]
                (self.playing.add if playing else self.playing.discard)(blk)
                print(f"  STATUS +{time.time() - self.t0:5.2f}s", decode_status(r))


def decode_input(r):
    x, y, thr, btn, b6, rud = struct.unpack_from("<HHBBBB", r, 1)
    hat = b6 >> 4
    return (f"X={x:4d} Y={y:4d} thr={thr:3d} rud={rud:3d} "
            f"hat={hat if hat < 8 else '-'} btn1-8={btn:08b} extra(bits48-51)={b6 & 0x0F:04b}")


def decode_status(r):
    f, blk = r[1], r[2]
    flags = [n for bit, n in enumerate(["actuator_power", "safety_switch", "actuators_enabled", "paused"])
             if f >> bit & 1]
    return f"raw={r.hex(' ')} flags={flags} block={blk & 0x7F} playing={blk >> 7}"


def cmd_info(dev, _):
    r = dev.feature(0x0B, 8)
    v = struct.unpack_from("<H", r, 1)[0]
    print("PID pool feature report 0x0B:", r.hex(" "))
    print(f"  RAM pool size         : {v & 0x7FFF}")
    print(f"  device managed pool   : {v >> 15}")
    print(f"  simultaneous effects  : {r[3]}")
    print(f"  usage 0x84 (raw)      : {r[4]}")
    print(f"  block sizes const/periodic/envelope/condition: {list(r[5:9])}")


def cmd_watch(dev, _):
    print("Move every axis to the stops, press every button incl. F1-F4 and shifts, roll the hat. Ctrl+C to stop.")
    last = None
    while True:
        r = dev.read(0.5)
        if not r:
            continue
        if r[0] == 0x06:
            s = decode_input(r)
            if s != last:
                print(r.hex(" "), "|", s)
                last = s
        elif r[0] == 0x02:
            print("STATUS", decode_status(r))
        else:
            print("OTHER", r.hex(" "))


def safe_stop(dev):
    print("Stopping: zero direct force, stop all, reset/disable")
    dev.send(0x81, 0, 0, quiet=True)
    dev.send(0x52, 2, quiet=True)  # desc: Stop All
    dev.send(0x52, 1, quiet=True)  # desc: Reset / linux: disable
    dev.send(0x43, 0, quiet=True)  # device gain 0


def cmd_stop(dev, _):
    safe_stop(dev)
    dev.pump(0.5)


def enable(dev, variant, gain):
    v = VARIANTS[variant]
    print(f"Gain {gain}/128, enable actuators ({variant})")
    dev.send(0x43, gain)
    dev.send(0x52, v["enable"])
    dev.pump(0.5)


def cmd_direct(dev, a):
    enable(dev, a.variant, a.gain)
    print(f"Direct force report 0x81: X={a.x} Y={a.y} for {a.time}s at 50 Hz")
    end = time.time() + a.time
    dev.send(0x81, a.x, a.y)
    while time.time() < end:
        dev.send(0x81, a.x, a.y, quiet=True)
        dev.pump(0.02)
    dev.send(0x81, 0, 0)


def cmd_hold(dev, a):
    """Enable actuators and do nothing else: feel what the firmware does on its own."""
    enable(dev, a.variant, a.gain)
    print(f"Holding {a.time}s with no effects loaded")
    dev.pump(a.time)


def cmd_vendor40(dev, a):
    """Vendor report 0x40: 8-bit index (1..16) + 16-bit value. Linux iforce uses the same
    command id for autocenter: index 3 = strength, index 4 = enable (its packet is 2 bytes)."""
    enable(dev, a.variant, a.gain)
    print("Feel the stick now (before 0x40)...")
    dev.pump(a.time)
    print(f"Vendor 0x40: index {a.index}, value {a.value}")
    dev.send(0x40, *struct.pack("<BH", a.index, a.value))
    print("...and now (after 0x40)")
    dev.pump(a.time)


# ---------- effect helpers (layouts from the descriptor, confirmed on hardware) ----------
TYPES = dict(constant=1, square=2, triangle=3, sine=4, saw_up=5, saw_down=6, spring=7, damper=8)
INFINITE = 0xFFFF
AX_X, AX_Y, DIR_EN = 1 << 6, 1 << 7, 1 << 5


def alloc(dev):
    """Lay out parameter blocks in device RAM using the sizes from feature report 0x0B."""
    sizes = dev.feature(0x0B, 8)[5:9]
    names = ["const", "periodic", "env", "cond_x", "cond_y"]
    sz = [sizes[0], sizes[1], sizes[2], sizes[3], sizes[3]]
    offs, cur = {}, 0
    for n, z in zip(names, sz):
        offs[n] = cur
        cur += (max(z, 1) + 3) // 4 * 4
    print(f"Block sizes const/periodic/env/cond = {list(sizes)}; offsets {offs}")
    return offs


def set_envelope(dev, off):
    dev.send(0x0C, *struct.pack("<HHBHB", off, 0, 127, 0, 127))


def set_effect(dev, block, etype, flags, duration_ms, direction, off1, off2):
    dev.send(0x0B, *struct.pack("<BBBHBHHHH", block, etype, flags, duration_ms, direction, 0, off1, off2, 0))


def play(dev, block, v, seconds):
    dev.send(0x51, block, v["start"], 1)
    dev.pump(seconds)
    dev.send(0x51, block, v["stop"], 0)


def cmd_sweep(dev, a):
    """Constant force with direction enabled on X+Y, stepping the direction byte."""
    v = VARIANTS[a.variant]
    offs = alloc(dev)
    enable(dev, a.variant, a.gain)
    set_envelope(dev, offs["env"])
    dev.send(0x0D, *struct.pack("<Hb", offs["const"], a.mag))
    for d in range(0, 256, 32):
        print(f"\n=== direction {d}/255 (~{d * 360 // 256} deg) -- note where the stick goes ===")
        set_effect(dev, 0, TYPES["constant"], DIR_EN | AX_X | AX_Y, 1200, d, offs["const"], offs["env"])
        play(dev, 0, v, 1.4)
        dev.pump(1.0)


def cmd_live(dev, a):
    """Infinite constant force on X, magnitude updated at 50 Hz as a slow sine: games do exactly this."""
    import math
    v = VARIANTS[a.variant]
    offs = alloc(dev)
    enable(dev, a.variant, a.gain)
    set_envelope(dev, offs["env"])
    dev.send(0x0D, *struct.pack("<Hb", offs["const"], 0))
    set_effect(dev, 0, TYPES["constant"], AX_X, INFINITE, 0, offs["const"], offs["env"])
    dev.send(0x51, 0, v["start"], 1)
    print(f"Updating magnitude at 50 Hz: {a.mag} * sin(2*pi*{a.freq}*t) for {a.time}s")
    t0 = time.time()
    while time.time() - t0 < a.time:
        m = int(a.mag * math.sin(2 * math.pi * a.freq * (time.time() - t0)))
        dev.send(0x0D, *struct.pack("<Hb", offs["const"], m), quiet=True)
        dev.pump(0.02)
    dev.send(0x51, 0, v["stop"], 0)


def cmd_condition(dev, a):
    """Spring or damper on both axes, one condition block per axis."""
    v = VARIANTS[a.variant]
    offs = alloc(dev)
    enable(dev, a.variant, a.gain)
    for axis in ("cond_x", "cond_y"):
        # offset, pos coef, neg coef, center offset, dead band, pos sat, neg sat
        dev.send(0x0F, *struct.pack("<HbbhHBB", offs[axis], a.coef, a.coef, 0, 0, 100, 100))
    print(f"{a.kind}: coef {a.coef}/100 on X and Y for {a.time}s. Push the stick around.")
    set_effect(dev, 0, TYPES[a.kind], AX_X | AX_Y, int(a.time * 1000), 0, offs["cond_x"], offs["cond_y"])
    play(dev, 0, v, a.time + 0.2)


def cmd_periodic(dev, a):
    """Periodic wave on X."""
    v = VARIANTS[a.variant]
    offs = alloc(dev)
    enable(dev, a.variant, a.gain)
    set_envelope(dev, offs["env"])
    # offset, magnitude 0..127, offset -127..127, phase 0..255, period ms
    dev.send(0x0E, *struct.pack("<HBbBH", offs["periodic"], a.mag, 0, 0, a.period))
    print(f"{a.wave}: magnitude {a.mag}, period {a.period} ms on X for {a.time}s")
    set_effect(dev, 0, TYPES[a.wave], AX_X, int(a.time * 1000), 0, offs["periodic"], offs["env"])
    play(dev, 0, v, a.time + 0.2)


def cmd_const(dev, a):
    v = VARIANTS[a.variant]
    pool = dev.feature(0x0B, 8)
    size_const, _, size_env, _ = pool[5:9]
    off_const, off_env = 0, max(size_const, 4)
    print(f"Block sizes const={size_const} env={size_env}; offsets const={off_const} env={off_env}")
    enable(dev, a.variant, a.gain)
    if a.ac_index is not None:
        print(f"Vendor 0x40: index {a.ac_index} = 0 (try to switch off the built-in centering spring)")
        dev.send(0x40, *struct.pack("<BH", a.ac_index, 0))
        dev.pump(0.3)

    print("Envelope 0x0C: no attack/fade, full levels")
    dev.send(0x0C, *struct.pack("<HHBHB", off_env, 0, 127, 0, 127))
    print(f"Constant 0x0D: magnitude {a.mag}")
    dev.send(0x0D, *struct.pack("<Hb", off_const, a.mag))

    # byte 2: trigger button (5 bits, 0 = none) | dir_enable << 5 | axisX << 6 | axisY << 7
    axes = {"x": 1 << 6, "y": 1 << 7, "xy": (1 << 6) | (1 << 7)}[a.axes]
    flags = (0 if a.no_dir else 1 << 5) | axes
    duration_ms = int(a.time * 1000)
    print(f"Set Effect 0x0B: block {a.block}, type 0x{a.type:02x}, axes {a.axes}, "
          f"dir_enable {not a.no_dir}, {duration_ms} ms, direction {a.dir}/255")
    dev.send(0x0B, *struct.pack("<BBBHBHHHH", a.block, a.type, flags, duration_ms, a.dir, 0,
                                off_const, off_env, 0))
    dev.pump(0.2)

    print("Effect Operation 0x51: start")
    dev.send(0x51, a.block, v["start"], 1)
    dev.pump(a.time + 0.5)
    print("Effect Operation 0x51: stop")
    dev.send(0x51, a.block, v["stop"], 0)
    dev.pump(0.3)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--i-know", action="store_true", help="allow stronger forces than the safe caps")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    sub.add_parser("watch")
    sub.add_parser("stop")
    for name in ("direct", "const", "hold", "vendor40", "sweep", "live", "condition", "periodic"):
        p = sub.add_parser(name)
        p.add_argument("--variant", choices=VARIANTS, default="desc")
        p.add_argument("--gain", type=int, default=32)
        p.add_argument("--time", type=float, default=1.0)
    sub.choices["direct"].add_argument("--x", type=int, default=15)
    sub.choices["direct"].add_argument("--y", type=int, default=0)
    sub.choices["const"].add_argument("--mag", type=int, default=30)
    sub.choices["const"].add_argument("--dir", type=int, default=64)
    sub.choices["const"].add_argument("--block", type=int, default=0)
    sub.choices["const"].add_argument("--type", type=lambda v: int(v, 0), default=1,
                                      help="effect type byte: 1 per descriptor, 0x00 per I-Force")
    sub.choices["const"].add_argument("--axes", choices=["x", "y", "xy"], default="xy")
    sub.choices["const"].add_argument("--no-dir", action="store_true", help="clear the direction-enable bit")
    sub.choices["const"].add_argument("--ac-index", type=int, default=None,
                                      help="send vendor 0x40 <index>=0 before the effect")
    sub.choices["sweep"].add_argument("--mag", type=int, default=60)
    sub.choices["live"].add_argument("--mag", type=int, default=80)
    sub.choices["live"].add_argument("--freq", type=float, default=0.5)
    sub.choices["condition"].add_argument("--kind", choices=["spring", "damper"], default="spring")
    sub.choices["condition"].add_argument("--coef", type=int, default=60)
    sub.choices["periodic"].add_argument("--wave", choices=["sine", "square", "triangle", "saw_up", "saw_down"],
                                         default="sine")
    sub.choices["periodic"].add_argument("--mag", type=int, default=60)
    sub.choices["periodic"].add_argument("--period", type=int, default=200)
    sub.choices["vendor40"].add_argument("--index", type=int, default=3)
    sub.choices["vendor40"].add_argument("--value", type=int, default=0)
    a = ap.parse_args()

    if not a.i_know:
        for k, cap in (("gain", MAX_SAFE_GAIN), ("mag", MAX_SAFE_MAG), ("x", MAX_SAFE_MAG), ("y", MAX_SAFE_MAG),
                       ("coef", MAX_SAFE_MAG)):
            if abs(getattr(a, k, 0)) > cap:
                sys.exit(f"--{k} above safe cap {cap}; add --i-know to override")
        if getattr(a, "time", 0) > 10:
            sys.exit("--time above 10 s; add --i-know to override")

    path = find_hidraw()
    print("Device:", path)
    dev = Dev(path)
    handlers = dict(info=cmd_info, watch=cmd_watch, stop=cmd_stop, direct=cmd_direct, const=cmd_const,
                    hold=cmd_hold, vendor40=cmd_vendor40, sweep=cmd_sweep, live=cmd_live,
                    condition=cmd_condition, periodic=cmd_periodic)
    try:
        handlers[a.cmd](dev, a)
    except KeyboardInterrupt:
        print()
    finally:
        if a.cmd not in ("info", "watch", "stop"):
            safe_stop(dev)


if __name__ == "__main__":
    main()
