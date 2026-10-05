# afterburner-ffb

Force feedback for the **Thrustmaster Top Gun Afterburner Force Feedback** joystick (USB `044F:B550`)
on modern Windows, without a kernel driver.

The joystick still works as a plain controller on Windows 10/11, but force feedback is dead:
the only driver around is a 2012 repack of a 2007 Saitek kernel driver that modern Windows
refuses to load (catalog mismatch, SHA-1 signing, RWX sections blocked by Memory Integrity).

This project replaces it with a **user-mode DirectInput effect driver**: a small COM DLL that
DirectInput loads into the game process. It translates DirectInput effects into the joystick's
own HID output reports. The stock Windows HID driver stays in place.

- no kernel code, no driver signing, no test mode
- Secure Boot and Memory Integrity stay on
- 64-bit and 32-bit games

> **Status: v0.1, experimental.** The protocol is fully mapped and verified on real hardware
> under Linux (`linux/afb_hid.py`). The Windows driver is new and is waiting for its first round of
> testing on Windows 11. Reports and logs are very welcome in issues.

## How it works

```
game ──DirectInput 8──▶ dinput8.dll ──IDirectInputEffectDriver──▶ afbffb.dll ──HID output reports──▶ stock hidclass/hidusb ──▶ joystick
```

The firmware is Immersion I-Force. Its native 851-byte HID report descriptor exposes the
I-Force command set as HID PID reports (Set Effect, Envelope, Constant, Periodic, Condition,
Effect Operation, Device Control, Device Gain) with host-managed effect memory. Windows' own
`pid.dll` cannot drive that variant, which is why force feedback is missing out of the box.
See [docs/protocol.md](docs/protocol.md).

## Install

1. If an old Saitek/Thrustmaster FFB driver was ever installed for this joystick, remove it:
   Device Manager → uninstall device → tick "delete the driver". The stock HID driver must be in use.
2. Download `afbffb-*.zip` from [Releases](../../releases) (or build it, see below) and unpack it.
3. Run `install.cmd` **as Administrator**. Both regsvr32 dialogs must report success.
4. Unplug and replug the joystick.
5. Run `fftest.exe` to check effects without a game. Pass a magnitude 0..10000 to change strength.

Every driver call and every HID report is logged to `%TEMP%\afbffb.log`.

To remove: `uninstall.cmd` as Administrator, then replug the joystick.

## Supported effects

| Effect | Hardware | Notes |
| --- | --- | --- |
| Constant force | yes | live magnitude updates work |
| Square, sine, triangle, sawtooth up/down | yes | with attack/fade envelopes |
| Spring | yes | |
| Damper | accepted | barely noticeable on hardware, under investigation |
| Ramp, inertia, friction | no | planned as host-side emulation |

## Linux tools

The protocol was reverse-engineered on Linux with the joystick on the stock `usbhid` driver:

- `linux/afb-diag.sh` collects descriptors, the native HID report descriptor, input captures and tries the kernel `iforce` driver
- `linux/afb_hid.py` talks to the joystick through `/dev/hidraw`: input monitor, pool info, and every effect type

```bash
sudo python3 linux/afb_hid.py info
sudo python3 linux/afb_hid.py watch
sudo python3 linux/afb_hid.py const --variant desc --mag 30
```

## Building

The Windows binaries are cross-built on Linux with mingw-w64 (CI does the same on every push):

```bash
sudo apt install g++-mingw-w64-x86-64 g++-mingw-w64-i686 zip
driver/build.sh            # -> driver/build/afbffb-dev.zip
```

## Credits

- The Linux kernel `iforce` driver (Vojtech Pavlik, Johann Deneux) was the Rosetta stone for the I-Force command set.

## Disclaimer

Not affiliated with Thrustmaster, Saitek or Immersion. Force feedback motors can move the stick
abruptly: keep fingers clear of the gimbal when testing. Use at your own risk.

## License

MIT
