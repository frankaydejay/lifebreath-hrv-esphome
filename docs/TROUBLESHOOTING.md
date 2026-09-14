# Validation and troubleshooting

## Validation

### Counters and frames

| Check | Expected |
| --- | --- |
| `HRV Raw byte count` | Rising continuously (~50/s) |
| `HRV TX send count` | Rising at ~4/s |
| `HRV Panel cycle count` | Rising at ~4.1/s |
| `HRV Panel raw byte count` | Rising at ~52-58/s (echo + panel + timers) |
| `HRV Panel RX count` | Rising at **exactly** the panel cycle rate (1 per cycle) |
| `HRV Timer RX count` | Rising at ~4/s |
| `HRV Timer values seen` | Only `FE:` and/or `00:` — anything else means echo leakage |
| `HRV Frame to panel` | e.g. Fresh 4 → `C7 67 DF 7B 5E 40 30 2B AE 9D F7 EF` |
| `HRV Panel raw dump` | `C7 [panel cmd] 67 DF 7B ... F7 [FE/00 timer] EF` |
| `YEL_PANEL` vs bus ground | 3.1-3.3 V |

### Control

| Action | Expected |
| --- | --- |
| HA control switch **ON** plus a setpoint | HRV follows within ~0.5 s |
| HA control switch **OFF** | HRV follows the panel within ~0.5 s |
| Press a bathroom timer | `HRV Bathroom timer active` → on in under 1 s |

### Boost

| Test | Expected |
| --- | --- |
| Press a timer | `HRV Boost active` → on, control source shows `Timer boost` |
| Change the HA setpoint **during** a boost | Recorded but not applied; the HRV holds the boost speed |
| Timer expires | Automatic return to the previous source |
| `HRV Boost last duration` | Roughly matches the timer that was pressed |

## Symptom table

| Symptom | Likely cause |
| --- | --- |
| `Raw byte count` freezes when the downstream RX is connected | Missing or misplaced 100 nF capacitor |
| `Panel raw byte count` stays at 0 | No bias on `YEL_PANEL`, miswired downstream 6N137, or a dead GPIO |
| Panel blinks green | Idle voltage too low — raise the RED-side resistor to 2.2 kΩ |
| HRV changes speed on its own | Echo filter too wide — tighten the window to 8000 / 17000 µs |
| `HRV Panel command` stuck at "—" | Window misaligned — inspect `HRV Panel raw dump timing` |
| `HRV Timer values seen` contains cycle bytes | Echo leakage — check `lcd_is_echo_byte()` |
| The HA UI overwrites itself while you set it | Grace period expired — raise `UI_GRACE_MS` |
| Boost engages spontaneously | Bus glitch — raise `BOOST_ENGAGE_SAMPLES` |
| Boost never releases | Check `HRV Timer byte`; the max-duration cap should eventually trip |
| Boost settings lost on reboot | `restore_value: true` missing, or combined with `initial_value` (compile error) |
| Compile error on `Select::state` | Recent ESPHome removed that member — use the UI mirrors in the header |

## The failure worth knowing about

During bring-up, connecting the downstream receiver caused **the HRV to stop
running entirely**. The chain of reasoning that found it:

1. Voltage measurements were fine — LED current was actually *higher* than on
   the working upstream segment, so bias was not the problem.
2. Disconnecting the output pin while leaving the LED powered **kept the HRV
   dead** — so the disturbance travelled through the power rail, not the signal.
3. `HRV Raw byte count` froze while logs kept flowing — reception on the
   **upstream** optocoupler was dying whenever the **downstream** one was
   connected.
4. Continuity and isolation checks all passed, ruling out wiring.

Root cause: the downstream 6N137 held at its switching threshold with no
decoupling capacitor, oscillating and polluting the shared 5 V rail.

Adding the two 100 nF capacitors and biasing from the 12 V rail fixed it
completely. **If something inexplicable happens, check those capacitors first.**

## Porting to another unit

The diagnostic entities exist for exactly this. To rebuild the lookup table:

1. Set the wall panel to each of the 12 states in turn.
2. For each state, read `HRV Raw dump` and note the `b1`, `b2` and `tail` bytes
   at their fixed positions in the cycle.
3. Fill in `FRAME_LOOKUP` in `hrv_component.h`.
4. Derive the command formula by watching which byte the panel inserts in the
   reply slot for each state — visible in `HRV Panel raw dump`.

`HRV Raw dump timing` gives inter-byte gaps in microseconds, which is how the
21 ms cadence and the 5.5 ms reply delay were established. If your unit differs,
adjust `LCD_BYTE_INTERVAL_US` and `HRV_RESPONSE_DELAY_US`.
