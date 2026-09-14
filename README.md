# LifeBreath HRV → Home Assistant (ESPHome)

Full bidirectional control of a **LifeBreath RNC6-ES** HRV (Heat Recovery Ventilator)
from Home Assistant, using an **ESP32-S3** as a protocol-aware proxy on the
proprietary 3-wire bus — **without removing the original wall panel** and
**without losing the bathroom timers**.

The RNC6-ES has no network interface of any kind. The only link between the unit
and its DXPL-03 wall panel is a proprietary 2000-baud open-collector serial bus.
This project reverse-engineers that bus and inserts an ESP32 in the middle of it.

> ⚠️ This is a hardware modification of a 120 V appliance's low-voltage control
> bus. It involves cutting a wire. See [Safety](#safety). No affiliation with
> LifeBreath / Airia Brands. Use at your own risk.

## Features

- ✅ Read the HRV's **actual** mode (Fresh / Recirculation) and fan speed (0-5)
- ✅ Control the HRV from Home Assistant
- ✅ Read what the **wall panel** is requesting, as a separate entity
- ✅ Runtime switch: **Home Assistant** or **wall panel** decides
- ✅ Optional auto-handover back to the panel when someone presses a button on it
- ✅ **Bathroom timer boost** — detects the timer switches and overrides both
  sources for the duration, restoring functionality that the wire cut removes
- ✅ Local OLED display (mode, speed, active source, timer state)
- ✅ Extensive diagnostic entities (raw byte dumps, timings, counters)

## Hardware

| Part | Qty | Purpose |
| --- | --- | --- |
| M5Stack Atom S3 Lite (ESP32-S3) | 1 | Controller |
| 6N137 high-speed optocoupler | 2 | Bus RX (upstream + downstream) |
| PC817 optocoupler | 2 | Bus TX (upstream + downstream) |
| 1N4148 diode | 2 | Reverse protection for 6N137 LEDs |
| 680 Ω resistor | 4 | 6N137 cathodes (×2), PC817 anodes (×2) |
| 10 kΩ resistor | 2 | Pull-ups for open-collector VO outputs |
| 1.5 kΩ resistor | 1 | Downstream bus bias from the 12 V rail |
| **100 nF ceramic capacitor** | **2** | **6N137 decoupling — MANDATORY** |
| SSD1306 128×64 I²C OLED | 1 | Local display |

> The two 100 nF capacitors are **not optional**. Without them the downstream
> 6N137 oscillates and corrupts the shared 5 V rail, which silently kills
> reception on the upstream optocoupler. See
> [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

## How it works

The HRV continuously emits a 12-byte cycle (~252 ms). The wall panel answers
inside a reply slot. The data wire is cut in two, and the ESP32 bridges both
halves in software:

```
   HRV RNC6-ES                    ESP32-S3                    DXPL-03 panel
                                                              + bath timers
  12-byte cycle ──► GPIO5 ──► decode mode/speed
                                    │
                                    ├──► regenerate 12 bytes ──► GPIO8 ──►
                                    │
  GPIO6 ◄── repeated command ◄── ARBITER ◄──┬── Bath timer boost  (prio 1)
       (anchored to 0xC7 + 5.5 ms)          ├── Home Assistant    (prio 2)
                                            └── Wall panel        (prio 2)
                                                     ▲
                                          GPIO7 ─────┴── panel + timers
```

Because the wire is physically cut, the arbitration is **fail-safe by
construction**: the panel can only reach the HRV through the ESP32.

Full protocol documentation: [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Installation

1. Wire the interface per [docs/WIRING.md](docs/WIRING.md). Take the reference
   measurements before powering the ESP32.
2. Copy `secrets.yaml.example` to `secrets.yaml` and fill in your Wi-Fi.
3. Put `hrv_component.h` next to `lifebreath-hrv.yaml`.
4. `esphome run lifebreath-hrv.yaml`
5. Adopt the device in Home Assistant.
6. Validate using [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md#validation).

## Entities

### Controls
| Entity | Description |
| --- | --- |
| `select.hrv_target_mode` | Fresh / Recirc setpoint |
| `number.hrv_target_speed` | Speed setpoint 0-5 (0 = Off) |
| `switch.hrv_home_assistant_control` | ON = HA commands, OFF = wall panel commands |
| `switch.hrv_wall_panel_priority` | Panel takes over automatically when touched |
| `switch.hrv_timer_boost_enabled` | Enable bathroom timer boost |

### Boost configuration (persistent)
| Entity | Default |
| --- | --- |
| `select.hrv_boost_mode` | Fresh |
| `number.hrv_boost_speed` | 5 (range 1-5, never 0) |
| `number.hrv_boost_max_duration` | 90 min (0 = unlimited) |

### State
| Entity | Description |
| --- | --- |
| `text_sensor.hrv_mode` / `sensor.hrv_fan_speed` | Actual HRV state |
| `text_sensor.hrv_state` | Combined, e.g. `Fresh 3` |
| `text_sensor.hrv_control_source` | `Home Assistant` / `Wall panel` / `Timer boost` |
| `text_sensor.hrv_bathroom_timer` | `None` / `Active` |
| `binary_sensor.hrv_bathroom_timer_active` | Raw timer signal |
| `binary_sensor.hrv_boost_active` | Boost engaged (post-debounce) |
| `sensor.hrv_boost_last_duration` | Duration of the previous boost |

Plus ~15 diagnostic entities (byte counters, raw dumps, timings).

## Known limitations

**The DXPL-03 panel's own display does not follow the HRV state.** The panel
behaves as a bus master: it keeps its selection in internal memory, displays it,
and transmits it in a loop regardless of what it receives. Regenerating a
perfectly formed cycle does not change this. Some field in the frame may carry
an "adopt this state" command, but it has not been identified.

Consequence: when Home Assistant is in control, the panel's screen shows a stale
value. The OLED and the HA dashboard show the truth, and a
`binary_sensor.hrv_panel_out_of_sync` entity flags the discrepancy.

**The three bathroom timer durations (20/40/60 min) cannot be told apart.** The
timers emit a variable-width low pulse, and 4.5 ms saturates the UART byte at
`0x00` for all three. This is worked around rather than solved: the timer holds
its signal for its whole duration, so the firmware simply boosts while the
signal lasts, and `sensor.hrv_boost_last_duration` reveals afterwards which one
was pressed.

## Safety

- Work with the HRV **unplugged** when cutting the wire and connecting.
- The bus side and the ESP32 side must stay **galvanically isolated**. Verify
  > 1 MΩ between the two grounds before powering up.
- **Never** connect 5 V directly to an ESP32-S3 GPIO (not 5 V tolerant).
- **Never** hard-wire a voltage source to the data line — it is open-collector,
  devices signal by pulling it **low**.
- Do not add a second ground reference in the bus domain (e.g. an external USB
  supply), it defeats the isolation.

## Adapting to other models

Only tested on an **RNC6-ES** with a **DXPL-03** panel. Other LifeBreath units
may share the bus but use a different lookup table. `docs/PROTOCOL.md` explains
how the table was built; the diagnostic dump entities give you everything you
need to rebuild it for your unit.

If you get it working on another model, a PR updating the table is welcome.
