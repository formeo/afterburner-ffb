# Thrustmaster Top Gun Afterburner FFB — USB protocol

Everything below was read from the device's own HID report descriptor and verified on hardware
through Linux `hidraw` unless marked otherwise. All multi-byte fields are little-endian.
Report IDs are the first byte of every report.

## Device

| Field | Value |
| --- | --- |
| VID:PID | `044F:B550`, "HOTAS Force Feedback Joystick" |
| Speed | USB 1.x full speed |
| Interfaces | 1, HID (class 03) |
| EP 0x82 IN | interrupt, 16 bytes, 8 ms |
| EP 0x01 OUT | interrupt, 32 bytes, 4 ms |
| Report descriptor | 851 bytes, includes the HID PID usage page (0x0F) |
| Firmware | Immersion I-Force (answers the I-Force `O/M/P/B/N` vendor queries, 20 effect slots) |

## Input reports

### 0x06 — controls (8 data bytes)

| Bits | Field | Range |
| --- | --- | --- |
| 0–15 | X | 0..4096 |
| 16–31 | Y | 0..4096 |
| 32–39 | Throttle | 0..255 |
| 40–47 | Buttons 1–8 | bitmask |
| 48–51 | padding | always 0 |
| 52–55 | Hat | 0..7, ≥ 8 = centred |
| 56–63 | Rudder | 0..255 |

### 0x02 — PID state (3 data bytes)

| Bits | Field |
| --- | --- |
| 0 | actuator power |
| 1 | safety switch |
| 2 | actuators enabled |
| 3 | device paused |
| 5 | set while vendor direct force (0x81) is active (observed, not in the descriptor) |
| 8–14 | effect block index |
| 15 | effect playing |

The device sends this report continuously, cycling the block index 0..19.

## Feature report 0x0B — PID pool (8 data bytes)

Observed: `0b e8 03 14 02 01 0c 0c 08`

| Field | Value |
| --- | --- |
| RAM pool size | 1000 |
| Device managed pool | 0 (host manages effect memory) |
| Simultaneous effects max | 20 |
| Parameter block sizes: constant / periodic / envelope / condition | 1 / 12 / 12 / 8 |

## Output reports

| ID | PID meaning | Data bytes | I-Force command (Linux `iforce.h`) |
| --- | --- | --- | --- |
| 0x0B | Set Effect | 14 | `0x010e` EFFECT |
| 0x0C | Set Envelope | 8 | `0x0208` ENVELOPE |
| 0x0D | Set Constant Force | 3 | `0x0303` MAGNITUDE |
| 0x0E | Set Periodic | 7 | `0x0407` PERIOD |
| 0x0F | Set Condition | 10 | `0x050a` CONDITION |
| 0x51 | Effect Operation | 3 | `0x4103` PLAY |
| 0x52 | Device Control | 1 | `0x4201` ENABLE |
| 0x43 | Device Gain | 1 | `0x4301` GAIN |
| 0x40 | vendor (index 1..16 + u16 value) | 3 | `0x4002` AUTOCENTER (semantics unknown, index 3 has no effect) |
| 0x44 | PID Pool Move (src, dst, len) | 6 | — (unused) |
| 0x81 | vendor direct force | 2 | — |

### 0x0B Set Effect

```
off  size  field
0    1     block index 0..19
1    1     type: 1 constant, 2 square, 3 triangle, 4 sine, 5 saw up, 6 saw down, 7 spring, 8 damper
2    1     bits 0-4 trigger button (0 = none), bit 5 direction enable, bit 6 axis X, bit 7 axis Y
3    2     duration, ms (0xFFFF = infinite)
5    1     direction 0..255 (256 steps per 360°)
6    2     trigger repeat interval, ms
8    2     offset 1: type-specific block (condition: X axis)
10   2     offset 2: envelope block (condition: Y axis)
12   2     start delay, ms
```

Type byte `0x00` also plays as constant force.

**Direction** follows DirectInput polar semantics ("where the force comes from", clockwise from north):
0 → pushes the stick toward the user, 64 → left, 128 → away, 192 → right.
With direction disabled and a single axis, a positive magnitude on X pushes left (DirectInput convention).

### 0x0C Set Envelope

```
0  2  parameter block offset
2  2  attack time, ms (0..10000)
4  1  attack level 0..127
5  2  fade time, ms (0..10000)
7  1  fade level 0..127
```

### 0x0D Set Constant Force

```
0  2  parameter block offset
2  1  magnitude, signed -127..127
```

Updating it while the effect plays changes the force live (verified at 50 Hz).

### 0x0E Set Periodic

```
0  2  parameter block offset
2  1  magnitude 0..127
3  1  offset, signed -127..127
4  1  phase 0..255
5  2  period, ms (0..10000)
```

### 0x0F Set Condition (one block per axis)

```
0  2  parameter block offset
2  1  positive coefficient -100..100
3  1  negative coefficient -100..100
4  2  centre point offset -500..500
6  2  dead band 0..1000
8  1  positive saturation 0..100
9  1  negative saturation 0..100
```

### 0x51 Effect Operation

```
0  1  block index
1  1  1 stop, 2 start, 3 start solo
2  1  loop count 0..254 (255 used for infinite by afbffb)
```

### 0x52 Device Control

`1` reset, `2` stop all, `3` enable actuators, `4` disable actuators, `5` pause, `6` continue.

Note: the Linux `iforce` driver sends `0x04` to *enable* — on this device that **disables** the actuators
(verified via the status report). Trust the descriptor.

### 0x43 Device Gain

One byte, 0..128.

### 0x81 Vendor direct force

```
0  1  X force, signed -127..127 (+X pushes the stick right)
1  1  Y force, signed -127..127
```

Note the sign is opposite to effects: it is "push toward", not "comes from".

## Behaviour notes

- No built-in centering spring: with actuators enabled and no effects the stick is free.
- Damper (type 8) is accepted and reports `playing=1` but is barely noticeable.
- Parameter offsets are in device memory units; afbffb gives each of the 20 blocks 48 units.
