# ESPHome Emporia Vue — RoussLabs mod

> ⚠️ **Please Note: This is currently a work in progress**\
> Directed and edited by RoussLabs; grunt work by robots.\
> Nothing here is anything I wouldn't read or reference myself.

A fork of [emporia-vue-local/esphome](https://github.com/emporia-vue-local/esphome), the unofficial ESPHome
component that runs the Emporia Vue 2 / Vue 3 energy monitor fully locally with Home Assistant.
All credit for the hardware reverse engineering and the base component goes to that project.

This is a standalone repository, not a GitHub fork: its first commit is a snapshot of upstream `dev` at
[`79e27473`](https://github.com/emporia-vue-local/esphome/commit/79e2747371b70370b96ef919f7c1e556f461344a)
(2026-04-18), and every change since is its own commit so it can be offered upstream one at a time.

Install instructions, flashing and panel wiring are unchanged: see the
[upstream docs site](https://emporia-vue-local.github.io/docs/tutorial/intro).

## Using this fork

```yaml
external_components:
  - source: github://RoussLabs/ESPHomeVueEmporiaMod@main
    components: [emporia_vue]
```

Every upstream option still works unchanged. The additions below are all optional and default to upstream behaviour.

## What's added

| # | Feature | Config key | Status |
|---|---------|-----------|--------|
| 1 | Apparent power, reactive power, power factor per CT | `ct_clamps[].apparent_power` / `reactive_power` / `power_factor` | done |
| 2 | Per-CT calibration gain (scales power **and** current) | `ct_clamps[].calibration` (default `1.0`) | done |
| 3 | Unconnected CT inputs (0xFFFF) publish NaN instead of garbage | automatic | done |
| 4 | Checksum discovery / validation | `checksum: none \| discover \| sum8_body \| …` | discover mode; algorithm not yet confirmed |
| 5 | Missed-reading detection fixed across the 255→0 sequence wrap | automatic | done |
| 6 | Raw period/angle averaging for finer frequency and phase-angle resolution | `frequency_samples: N` (default `1`) | done |
| 7 | Diagnostic counters | `i2c_errors`, `malformed_readings`, `missed_readings` | done |
| 8 | Virtual (line-to-line) phases for 240 V split-phase / 208 V circuits with one CT | `virtual_phases:` | done, needs field testing |
| 9 | `dump_config` lists every sensor; minor cleanups | — | done |

### 1. Apparent power, reactive power, power factor

The metering MCU reports real power (P) and true-RMS current (I) per CT and RMS voltage (V) per leg. The
component now derives, from the same 200 ms reading:

- `apparent_power` S = V × I (VA)
- `reactive_power` Q = √(S² − P²) (var)
- `power_factor` PF = P / S (true power factor, so it includes distortion from switch-mode loads)

PF is published as NaN below 1 VA, where the ratio is noise.

```yaml
ct_clamps:
  - phase_id: phase_a
    input: "3"
    power:          { name: "Fridge Power" }
    current:        { name: "Fridge Current" }
    apparent_power: { name: "Fridge Apparent Power" }
    power_factor:   { name: "Fridge Power Factor" }
```

### 2. Per-CT calibration

Upstream has one `calibration` per leg that scales voltage and every power reading on that leg together, and
current had no calibration at all. Each CT now takes its own gain, applied to both power and current so PF stays
consistent:

```yaml
  - { phase_id: phase_a, input: "5", calibration: 1.013, power: { id: cir5 } }
```

### 3. Disconnected inputs

The Vue 3 reports `0xFFFF` current on a port with no CT. Those ports now publish NaN (and log once) instead of
nonsense. You can still remove unused entries from the config; this just stops bad data if you forget.

### 4. Checksum

The packet header carries a checksum byte but the algorithm isn't publicly documented. Set `checksum: discover`
and watch the log for 10 lines like:

```
checksum=0x3A unknown=0x52 seq=17 | body: sum8=0x3A xor8=0x91 negsum8=0xC6 | all: sum8=0x2F xor8=0x4C negsum8=0xD1
```

If one candidate matches the packet value every time, set `checksum:` to that name (`sum8_body` in the example)
and bad packets will be discarded and counted in `malformed_readings`. If nothing matches, leave it at `none` and
please open an issue with a few log lines.

### 6. Frequency / phase-angle resolution

The MCU gives the mains period as an integer count (~327 at 60 Hz on the Vue 3), so one reading resolves to
about 0.18 Hz and 1°. `frequency_samples: 25` averages 25 raw counts (~6 s) before converting, for roughly
0.01 Hz. This is why upstream readings of "60.3 Hz" look wrong: real grid frequency is held within ±0.04 Hz.

### 7. Diagnostics

```yaml
sensor:
  - platform: emporia_vue
    i2c_errors:         { name: "Vue I2C Errors" }
    malformed_readings: { name: "Vue Malformed Readings" }
    missed_readings:    { name: "Vue Missed Readings" }
```

All three are `total_increasing` diagnostic entities that reset at boot. Useful when tuning the I2C `frequency`
(users report 150–400 kHz behaving differently on the Vue 3).

### 8. Virtual phases (240 V / 208 V with one CT)

For a pure 240 V load (water heater, baseboard heat, EVSE) measured by a single CT on one leg:

```yaml
    phases:
      - { id: phase_a, input: BLACK, calibration: 0.01925, voltage: { name: "L1 Voltage" } }
      - { id: phase_b, input: RED,   calibration: 0.01925, voltage: { name: "L2 Voltage" } }
    virtual_phases:
      - id: leg_1_2
        combine: [phase_a, phase_b]
        voltage: { name: "L1-L2 Voltage" }
    ct_clamps:
      - { phase_id: leg_1_2, input: "4", power: { name: "Water Heater Power" } }
```

Power is computed as P<sub>a</sub> − P<sub>b</sub> from the MCU's per-leg real-power columns, which is exact
for any angle between the legs and replaces the old `multiply: 2` approximation. Voltage is |V<sub>a</sub> −
V<sub>b</sub>| by the law of cosines using the measured phase angle, so a 120° (208 V) service needs no special
configuration.

Only use this for loads with **no neutral current**. A dryer or range with 120 V controls still needs one CT per
leg, summed in a template sensor.

## Calibrating with a true-RMS meter

A Fluke 87 (or similar, ±0.7 % V / ±1 % A) is better than the component's defaults. Do voltage first, then
current; use the 5 s throttled sensors and average a few readings because both the meter and the Vue drift.

1. **Voltage, per leg.** Read L1–N and L2–N at a lightly loaded moment while watching the HA voltage sensors.
   `new_calibration = old_calibration × V_meter / V_vue`. Repeat once.
2. **Current / power, per CT.** Run a purely resistive load (toaster, heater on low; keep under 10 A for the
   meter's fused input) through a split extension cord with the meter in series on the hot. Then
   P<sub>true</sub> = V<sub>meter</sub> × I<sub>meter</sub>. Set that CT's `calibration` so `power` matches;
   `current` and `power_factor` follow automatically. For the 200 A mains CTs, loop the conductor through the
   clamp 4× to get a usable signal from a bench load (reading = 4 × actual).
3. **Sanity check.** A resistive load should show `power_factor` ≈ 1.00. Anything far off means the current
   scalar is wrong, not the load.

## Example

[`example/vue3.yaml`](example/vue3.yaml) is a complete Vue 3 config using every option in this fork.

## Upstream status

| Commit | Upstream PR |
|--------|-------------|
| all | not yet submitted |

## License

Unchanged from upstream: the [ESPHome License](LICENSE) (GPLv3 for the C++ files, MIT for Python and everything
else).
