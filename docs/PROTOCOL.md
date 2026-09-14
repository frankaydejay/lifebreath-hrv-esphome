# LifeBreath RNC6-ES bus protocol

Reverse-engineered from an RNC6-ES paired with a DXPL-03 wall panel. Everything
here was verified empirically; nothing comes from manufacturer documentation.

## Physical layer

| Property | Value |
| --- | --- |
| Wires | RED (+12 V), YEL (data), GRN (ground) |
| Idle data voltage | ~3.18 V |
| Signalling | Open collector — devices pull the line **low** |
| Baud rate | 2000 |
| Framing | 8N1 |
| Polarity | Inverted relative to standard UART (`UART_SIGNAL_RXD_INV` / `TXD_INV`) |

Because idle is high and devices assert by pulling low, the bus behaves as a
wired-OR: simultaneous assertions merge into the longest one.

## Frame structure

The HRV transmits a 12-byte cycle continuously, one byte every **21 ms**
(~252 ms per cycle):

```
C7  [CMD]  67 DF 7B  [TAIL] 40 30 2B [B1] [B2]  F7 EF
 ▲     ▲                  └────────── 8-byte anchor ──────────┘
 │     └── reply slot: the panel (or the ESP32) inserts its command here
 └── anchor byte / implicit poll
```

- `TAIL` = `0x5E` (Fresh) or `0x5D` (Recirculation)
- `B1`, `B2` encode mode and fan speed together
- `0xBF` is **not** a sync marker; it is the actual "Off" command value

## State decoding table

| b1 | b2 | tail | Mode | Speed |
| --- | --- | --- | --- | --- |
| 0xAF | 0x9F | 0x5E | Fresh | 0 (Off) |
| 0xAF | 0x99 | 0x5E | Fresh | 1 |
| 0xAF | 0x95 | 0x5E | Fresh | 2 |
| 0xAF | 0x91 | 0x5E | Fresh | 3 |
| 0xAE | 0x9D | 0x5E | Fresh | 4 |
| 0xAE | 0x99 | 0x5E | Fresh | 5 |
| 0xAF | 0x9F | 0x5D | Recirc | 0 (Off) |
| 0xAF | 0x9B | 0x5D | Recirc | 1 |
| 0xAF | 0x97 | 0x5D | Recirc | 2 |
| 0xAF | 0x93 | 0x5D | Recirc | 3 |
| 0xAE | 0x9F | 0x5D | Recirc | 4 |
| 0xAE | 0x9B | 0x5D | Recirc | 5 |

## Command encoding

```
Fresh  : cmd = 0x7F − 8 × speed    (1=0x77, 2=0x6F, 3=0x67, 4=0x5F, 5=0x57)
Recirc : cmd = 0x3F − 8 × speed    (1=0x37, 2=0x2F, 3=0x27, 4=0x1F, 5=0x17)
Off    : 0xBF                      (special value, outside the formula)
```

## Commanding the HRV

Two requirements, both mandatory:

1. **Anchoring** — detect the HRV's `0xC7` byte, then transmit **exactly
   5.5 ms** later. The implementation spin-waits rather than using a timer,
   because jitter here causes the HRV to ignore the command.
2. **Repetition** — the HRV requires the same command across several
   consecutive cycles before it acts on it.

Observed latency from command to fan change: about 0.5 s.

## Bathroom timers

The timer switches sit on the same bus as the panel, wired in parallel.

**They do not transmit bytes.** They pull the line low for a variable duration;
the UART frames that pulse into a byte whose trailing zero count encodes the
width:

```
width ≈ (trailing_zero_bits + 1) × 0.5 ms     (start bit included, 2000 baud)
```

| Byte | Zero bits | Pulse width | Meaning |
| --- | --- | --- | --- |
| `0xFE` | 1 | 1.0 ms | Idle — no timer running |
| `0x00` | 8 | ≥ 4.5 ms (saturated) | At least one timer running |

### The three durations cannot be distinguished

Tested systematically:

| Condition | Byte | Width |
| --- | --- | --- |
| Idle | `0xFE` | 1.0 ms |
| 20 min timer | `0x00` | 4.5 ms |
| 40 min timer | `0x00` | 4.5 ms |
| 60 min timer | `0x00` | 4.5 ms |
| 60 min, 5 minutes into the countdown | `0x00` | 4.5 ms |
| Two timers simultaneously | `0x00` | 4.5 ms |

The byte **saturates**: past 4.5 ms every bit is already zero, so the UART
cannot measure further. The value also does not change during the countdown, so
it does not encode remaining time. Two timers at once give `0x00` as well,
consistent with wired-OR behaviour.

**The signal is purely binary: "at least one timer running" or "none".**

Distinguishing the durations would require abandoning the UART and timing the
falling edge with a GPIO interrupt. This project does not bother, because:

### Duration is recoverable from persistence

The timer holds its pulse for its **entire** selected duration, then releases.
Verified: a 20 min timer released its signal at T+20 min.

So the firmware simply boosts while the signal lasts — the timer performs its
own countdown. `sensor.hrv_boost_last_duration` then reports how long it ran,
which identifies which timer was pressed after the fact.

Sampling rate: exactly **one timer byte per ~250 ms bus cycle** (~4/s).

### Timer bytes drift across the frame

Offset of the timer byte after the `F7` echo, over nine consecutive cycles:

```
10556, 3993, 5002, 4994, 10560, 5003, 5001, 5005, 10563 µs
```

Interval between successive timer bytes: 246.4 / 252.0 / 252.0 / 257.6 ms,
against our regenerated cycle of exactly 252.0 ms. The timers run on their own
clock, so their position slides and can eventually land anywhere in the frame.

**This is why timer bytes are identified by echo exclusion rather than by a
time window.** A byte on the downstream RX is a timer byte if it is not one of
the twelve bytes we are currently transmitting, and it is not a valid command
inside the panel reply window. None of our cycle bytes equal `0x00` or `0xFE`,
so there is no ambiguity.

## Self-echo on the downstream bus

The downstream segment is a single wire, so everything transmitted on the TX
optocoupler is received back on the RX optocoupler. Critically, **the second
byte of the cycle, `0x67`, is also a valid "Fresh 3" command**, so naive
decoding would make the HRV chase its own echo.

Panel commands are therefore accepted only inside a time window measured from
our own `0xC7`:

| Byte | Arrival | Verdict |
| --- | --- | --- |
| Echo of our `C7` | ~5 ms | Rejected (< 6 ms) |
| **Panel reply** | **~10-11 ms** | **Accepted** |
| Echo of our `67` | ~25 ms | Rejected (> 19 ms) |
| Timer byte | variable | Accepted via echo exclusion |

Correct behaviour shows as `HRV Panel RX count` rising at exactly the same rate
as `HRV Panel cycle count` — one acceptance per cycle.

## Unresolved: the panel display

The DXPL-03 acts as a bus master. It stores its selection internally, displays
it, and transmits it in a loop, regardless of the frames it receives. Feeding it
a perfectly formed cycle carrying a different state does not update its screen.

Unexplored leads:
- An unidentified field may carry an "adopt this state" instruction. Candidates
  include `67 DF 7B`, or a variant of `40 30 2B` — a `40 34 2B` deviation was
  observed once while changing the dehumidistat threshold.
- The panel may expose a slave/display mode via some button sequence.
