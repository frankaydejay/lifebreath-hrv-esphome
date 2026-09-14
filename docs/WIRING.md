# Wiring

> Work with the HRV **unplugged**. Verify isolation before applying power.

## Principle

Only the **YEL (data)** wire is cut, splitting it into two segments:
`YEL_HRV` (toward the unit) and `YEL_PANEL` (toward the wall panel and timers).
**RED and GRN stay continuous**, giving both sides a shared 12 V rail and a
common bus ground.

Cutting the data wire is what makes arbitration fail-safe: the panel and the
timers physically cannot reach the HRV except through the ESP32.

```
                 ┌──────────────────────────────────────────┐
   HRV RNC6-ES   │              ESP32-S3                     │   Panel + timers
   (RED/YEL/GRN) │                                          │   (RED/YEL/GRN)
        ▼        │  UART1 "HRV bus"    UART2 "panel bus"     │        ▼
  ──────┬──────► │  RX=G5  TX=G6       RX=G7  TX=G8          │ ◄──┬──────
        │  YEL_HRV (cut here)                    YEL_PANEL (cut here)
  RED ──┴────────┼──────────────(continuous, not cut)────────┼────┴── RED
  GRN ──┴────────┼──────────────(continuous, not cut)────────┼────┴── GRN
                 │          OLED SSD1306 (I2C G38/G39)       │
                 └──────────────────────────────────────────┘
```

Galvanic isolation between the bus ground and the ESP32 ground is provided
**solely** by the optocouplers. Verify > 1 MΩ between them.

## 6N137 pinout

| Pin | Function |
| --- | --- |
| 1 | NC |
| 2 | LED anode (bus side) |
| 3 | LED cathode (bus side) |
| 4 | NC |
| 5 | **GND** (ESP side) |
| 6 | **VO** — open-collector output |
| 7 | **VE** — enable, active high |
| 8 | **VCC** |

**Tie pins 7 and 8 together to the Atom's 5 V rail.** VE high enables the
output; VCC powers the die.

## Upstream segment (HRV side)

```
BUS DOMAIN:
  YEL_HRV ──►│1N4148│──► 6N137 #1 pin 2 (LED anode)
                         6N137 #1 pin 3 (cathode) ──[680Ω]── BUS GND

ESP DOMAIN (isolated):
  6N137 #1 pin 5 (GND)  ──► ESP GND
  6N137 #1 pin 6 (VO)   ──► GPIO5   + [10kΩ] to 3V3
  6N137 #1 pin 7 (VE)   ──┬─► 5V
  6N137 #1 pin 8 (VCC)  ──┘
  [100 nF ceramic] between pin 8 and pin 5, leads < 1 cm

TX: GPIO6 ──[680Ω]──► PC817 #1 pin 1 (anode)
                      PC817 #1 pin 2 (cathode)   ──► ESP GND
                      PC817 #1 pin 4 (collector) ──► YEL_HRV
                      PC817 #1 pin 3 (emitter)   ──► BUS GND
```

The HRV biases this segment itself (open-circuit ~3.9-4.5 V, effective internal
impedance ~900-960 Ω). **Do not go below 680 Ω on the cathode resistor here.**

Expected idle voltage on `YEL_HRV`: 3.13-3.19 V → LED current ≈ 1.7 mA.

## Downstream segment (panel side)

Once the wire is cut, `YEL_PANEL` has **no bias source**. It must be recreated.
Use the **RED 12 V rail**, which is already in the bus domain — no external
supply, so isolation is preserved.

```
BUS DOMAIN:
  RED (~12 V) ──[ 1.5 kΩ ]──┬── PC817 #2 pin 4 (collector)
                             ├──►│1N4148│──► 6N137 #2 pin 2 (LED anode)
                             │               6N137 #2 pin 3 ──[680Ω]── BUS GND
                             └── YEL_PANEL ──► panel + timers
  PC817 #2 pin 3 (emitter) ──► BUS GND

ESP DOMAIN (isolated):
  6N137 #2 pin 5 (GND)  ──► ESP GND
  6N137 #2 pin 6 (VO)   ──► GPIO7   + [10kΩ] to 3V3
  6N137 #2 pin 7 (VE)   ──┬─► 5V
  6N137 #2 pin 8 (VCC)  ──┘
  [100 nF ceramic] between pin 8 and pin 5, leads < 1 cm   ← CRITICAL

  GPIO8 ──[680Ω]──► PC817 #2 pin 1 (anode)
                    PC817 #2 pin 2 (cathode) ──► ESP GND
```

## ⚠️ The 100 nF capacitors are mandatory

The 6N137 datasheet states plainly: *"A 0.1 µF bypass capacitor **must** be
connected between pins 5 and 8."*

- **One per 6N137**, so **two total**. The PC817 does not need one.
- Ceramic, marked `104`, non-polarised.
- Between **pin 8 (VCC)** and **pin 5 (GND)**, leads **under 1 cm**.
- On breadboard, since pins 7 and 8 are tied, you may span the pin 7 row to the
  pin 5 row. Make sure nothing touches the **pin 6 (VO)** row.

**Why this matters.** The 6N137 is a fast, high-gain comparator with no
hysteresis. On the downstream segment its LED sits at a *static* bias right at
the switching threshold (~1.9 mA, below the 5 mA the datasheet guarantees).
Undecoupled, the part oscillates at high frequency and pollutes the shared 5 V
rail, which corrupts reception on the **upstream** optocoupler. The visible
symptom is the HRV shutting down with `HRV Raw byte count` frozen — a failure
that looks nothing like its cause. The upstream part never shows this because
its LED is constantly switching rather than resting at threshold.

## Reference measurements

Take these **relative to bus ground (GRN wire)**, never the ESP ground — the two
grounds are floating with respect to each other.

| Measurement | Reference | Expected |
| --- | --- | --- |
| RED, unloaded | BUS GND | ~12 V |
| RED, fully connected | BUS GND | No sag |
| `YEL_PANEL` idle | BUS GND | 3.1-3.3 V |
| `YEL_HRV` idle | BUS GND | 3.13-3.19 V |
| Pin 8 vs pin 5, both optos | ESP GND | ~5.0 V |
| ESP GND ↔ BUS GND isolation | — | > 1 MΩ |
| Ohmmeter pin 8 ↔ pin 5, unpowered | — | > 100 kΩ, and climbing |

Downstream current budget (1.5 kΩ + 680 Ω, line at 3.18 V):
- Total through the 1.5 kΩ: (12 − 3.18) / 1500 ≈ 5.9 mA
- 6N137 LED current: (3.18 − 2.2) / 680 ≈ 1.44 mA
- Remainder to the panel: ≈ 4.4 mA → apparent panel impedance ≈ 720 Ω

## Fallback values

| Situation | Fix |
| --- | --- |
| Panel blinks green (comm error) | Raise the RED-side resistor to 2.2 kΩ |
| RED sags under load | 2.2 kΩ + 270 Ω |
| Want a firmer LED current (> 4 mA) | 1.1 kΩ + 220 Ω → node ≈ 3.06 V |

Unlike the upstream segment, the downstream cathode resistor **can** go down to
220 Ω, because you are supplying the bias yourself. Not needed in practice.

## What not to do

- **Do not** bias the line from an external USB supply with its ground on the
  bus. That adds a second ground reference, creating a ground loop and
  defeating the isolation. The RED rail is already available and robust.
- **Never** connect 5 V directly to an ESP32-S3 GPIO — it is not 5 V tolerant.
- **Never** hard-wire a voltage source to the data line. It is open-collector:
  devices communicate by pulling it **low**.
