# ESPHome Emporia Vue — RoussLabs mod

> ⚠️ **Please Note: This is currently a work in progress and is not ready yet. Expect many bugs.**\
> Directed and edited by RoussLabs; grunt work by robots.\
> Nothing here is anything I wouldn't read or reference myself.

A fork of [emporia-vue-local/esphome](https://github.com/emporia-vue-local/esphome), the unofficial ESPHome
component that runs the Emporia Vue 2 / Vue 3 energy monitor fully locally with Home Assistant.
All credit for the hardware reverse engineering and the base component goes to that project.

This is a standalone repository, not a GitHub fork: its first commit is a snapshot of upstream `dev` at
[`79e27473`](https://github.com/emporia-vue-local/esphome/commit/79e2747371b70370b96ef919f7c1e556f461344a)
(2026-04-18), and every change since is its own commit so it can be offered upstream one at a time.

Panel wiring is unchanged from upstream: see the
[upstream docs site](https://emporia-vue-local.github.io/docs/tutorial/intro). Flashing is summarised below.

## Flashing the Vue with ESPHome

> ⚠️ Never flash with the Vue connected to mains. Open the case only with every wire disconnected, and
> close it before it goes back in the panel. Panel work is for a qualified person.

Once ESPHome is on the device the Emporia app and cloud no longer work; the backup made in step 3 is the only way
back.

### 1. What you need

- A USB-to-TTL serial adapter, 3.3 V logic (CP2102, CH340 or FT232). It must supply enough current for an ESP32
  in bootloader mode; some cheap adapters can't, and the symptom is a chip that never enumerates.
- [esptool](https://docs.espressif.com/projects/esptool/en/latest/esp32/installation.html) on a computer with a
  USB port (`pip install esptool`).
- A running ESPHome Device Builder (Home Assistant add-on or standalone).
- For the **Vue 3**: a BDM frame with pogo pins, or jumper wires and a fine soldering iron. The pads are small and
  lift easily; the BDM frame is the safer option and several people have lost boards soldering.
- For the **Vue 2**: a 7-pin pogo clip or header pins soldered to the programming port.

### 2. Connect the serial adapter

**Vue 3**: remove the five screws on the back, loosen the SMA antenna nut, pry the halves apart. Five test pads
on the board, left to right as labelled in the
[upstream photo](https://emporia-vue-local.github.io/docs/tutorial/hardware-prep-v3): GND, 3.3 V, GND, TX, RX.
The RX/TX silkscreen is from the ESP32's point of view, so connect **TX to the adapter's TX and RX to RX**
(the reverse of the usual crossover). If it won't respond, swap them. Hold IO0 to ground (or use an adapter
with DTR/RTS wired to IO0/EN) while applying power so the chip starts in bootloader mode. Apply **3.3 V** to the
3.3 V pad last; 5 V on that pad destroys the board.

**Vue 2**: the programming header is a proper 2.54 mm port; see the
[upstream Vue 2 page](https://emporia-vue-local.github.io/docs/tutorial/hardware-prep-v2) for the pinout. It
takes 5 V on the `VCC_5V0` pin.

### 3. Back up the factory firmware

Do this before anything else; it is the only way to restore the Emporia firmware. Replace the port with yours
(`/dev/ttyUSB0`, `/dev/cu.usbserial-xxxx`, `COM5`):

```sh
esptool version                                   # confirms the install
esptool --port /dev/ttyUSB0 read-flash 0 ALL vue-orig-backup.bin
```

The Vue 3 has 8 MB of flash, so the file will be 8,388,608 bytes. If the read stalls at the same percentage on
Apple Silicon, use a different computer or add `--baud 115200`. Keep the file somewhere safe.

### 4. First flash: a minimal ESPHome image over serial

The first install is a placeholder; the real configuration is pushed over Wi-Fi afterwards.

1. In the Device Builder, **New Device**, give it a name, choose **ESP32**, then **Install → Plug into this
   computer** (or "the computer running ESPHome Device Builder" if the adapter is on that machine).
2. The Vue should be in bootloader mode (step 2) when the install starts. Wait for it to finish and reboot.
3. If the chip is not found: check TX/RX, check that IO0 was grounded at power-on, check the adapter's 3.3 V
   supply, and retry at a lower baud rate.

Serial alternative to the dashboard, from the folder holding your YAML:

```sh
esphome run emporia-vue-3.yaml --device /dev/ttyUSB0
```

### 5. Full configuration over Wi-Fi

1. Copy [`example/vue3.yaml`](example/vue3.yaml) over the placeholder config (keep the API encryption key the
   wizard generated, or set your own).
2. Fill in the circuit names and `phase_id` for each breaker from your panel notes.
3. **Install → Wirelessly**. All later changes go the same way; the serial wires are not needed again.

The first boot on the bench will log `I2C error` lines until the Vue is powered from the panel, because the
metering MCU is unpowered without mains. That is expected.

### 6. Restoring the factory firmware

If you ever want the Emporia app back:

```sh
esptool --port /dev/ttyUSB0 write-flash 0 vue-orig-backup.bin
```

## Using this fork

```yaml
external_components:
  - source: github://RoussLabs/ESPHomeVueEmporiaMod@main
    components: [emporia_vue]
    # refresh: 0s   # while testing: re-fetch the component on every build (default is 1 day)
```

Every upstream option still works unchanged. The additions below are all optional and default to upstream behaviour.

## What's added

| # | Feature | Config key | Status |
|---|---------|-----------|--------|
| 1 | Apparent power, reactive power, power factor per CT | `ct_clamps[].apparent_power` / `reactive_power` / `power_factor` | done |
| 2 | Per-CT calibration gain (scales power **and** current) | `ct_clamps[].calibration` (default `1.0`) | done |
| 3 | Unconnected CT inputs (0x5000 on the Vue 3) publish NaN instead of garbage | automatic | verified on hardware |
| 4 | Packet CRC-8 validation | `checksum: none \| discover \| crc8` | algorithm identified and verified on Vue 3 |
| 5 | Missed-reading detection fixed across the 255→0 sequence wrap | automatic | done |
| 6 | Raw period/angle averaging for finer frequency and phase-angle resolution | `frequency_samples: N` (default `1`) | done |
| 7 | Diagnostic counters | `i2c_errors`, `malformed_readings`, `missed_readings` | done |
| 8 | Virtual (line-to-line) phases for 240 V split-phase / 208 V circuits with one CT | `virtual_phases:` | done, needs field testing |
| 9 | `dump_config` lists every sensor; minor cleanups | — | done |

### 1. Apparent power, reactive power, power factor

**The short version, for anyone new to this.** With AC, voltage and current are waves, and a load can draw
current that is out of step with the voltage. That gives four related numbers:

- **Real power (W)** is the energy actually consumed per second. It's what the utility bills and what heats
  the room. A 1500 W heater uses 1500 W, full stop.
- **Apparent power (VA)** is simply volts × amps. It's the capacity the wiring, breaker and transformer must
  carry, whether or not the energy is used.
- **Reactive power (var)** is the part of the current that sloshes back and forth each cycle without being
  consumed: motors, transformers and the filters inside electronics cause it. It shows up as amps on the
  wire but not as watts on the bill.
- **Power factor** is real ÷ apparent, from 0 to 1. A heater or incandescent bulb is 1.00: every amp does
  work. A fridge compressor might be 0.7, a cheap LED driver or phone charger 0.5 or worse, meaning they draw
  roughly twice the current their wattage suggests.

Why care, at home: PF explains why a circuit's amps don't match its watts, which devices are stressing a
circuit more than their consumption implies, and whether a reading is trustworthy (a resistive load that
doesn't show PF ≈ 1 means the calibration is off, not the load).

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

The Vue 3 reports a current value of exactly `0x5000` on a port with no CT (verified: plugging a CT into a port
drops it to a normal reading while the others stay at `0x5000`). Those ports now publish NaN (and log once) instead of
nonsense. You can still remove unused entries from the config; this just stops bad data if you forget.

### 4. Checksum

The packet header carries a checksum byte that upstream reads but never checks. It is a **CRC-8, polynomial
0x07, initial value 0x1D, no reflection, no final XOR, over every byte after the checksum field** (bytes 2 to
283). This was found by brute force against four packets captured from a Vue 3 and matches all of them.

- `checksum: crc8` verifies every packet and discards mismatches, counting them in `malformed_readings`.
- `checksum: discover` logs the check result together with the raw per-port values and the whole packet in hex
  (first 10 packets, then once a minute). Useful for studying the protocol or if a Vue 2 turns out to differ.
- `checksum: none` (default) is upstream behaviour.

If `crc8` reports mismatches on a Vue 2, please open an issue with a few `discover` captures.

### 6. Restoring the factory firmware

If you ever want the Emporia app back:

```sh
esptool --port /dev/ttyUSB0 write-flash 0 vue-orig-backup.bin
```

## Using this fork

```yaml
external_components:
  - source: github://RoussLabs/ESPHomeVueEmporiaMod@main
    components: [emporia_vue]
    # refresh: 0s   # while testing: re-fetch the component on every build (default is 1 day)
```

Every upstream option still works unchanged. The additions below are all optional and default to upstream behaviour.

## What's added

| # | Feature | Config key | Status |
|---|---------|-----------|--------|
| 1 | Apparent power, reactive power, power factor per CT | `ct_clamps[].apparent_power` / `reactive_power` / `power_factor` | done |
| 2 | Per-CT calibration gain (scales power **and** current) | `ct_clamps[].calibration` (default `1.0`) | done |
| 3 | Unconnected CT inputs (0x5000 on the Vue 3) publish NaN instead of garbage | automatic | verified on hardware |
| 4 | Packet CRC-8 validation | `checksum: none \| discover \| crc8` | algorithm identified and verified on Vue 3 |
| 5 | Missed-reading detection fixed across the 255→0 sequence wrap | automatic | done |
| 6 | Raw period/angle averaging for finer frequency and phase-angle resolution | `frequency_samples: N` (default `1`) | done |
| 7 | Diagnostic counters | `i2c_errors`, `malformed_readings`, `missed_readings` | done |
| 8 | Virtual (line-to-line) phases for 240 V split-phase / 208 V circuits with one CT | `virtual_phases:` | done, needs field testing |
| 9 | `dump_config` lists every sensor; minor cleanups | — | done |

### 1. Apparent power, reactive power, power factor

**The short version, for anyone new to this.** With AC, voltage and current are waves, and a load can draw
current that is out of step with the voltage. That gives four related numbers:

- **Real power (W)** is the energy actually consumed per second. It's what the utility bills and what heats
  the room. A 1500 W heater uses 1500 W, full stop.
- **Apparent power (VA)** is simply volts × amps. It's the capacity the wiring, breaker and transformer must
  carry, whether or not the energy is used.
- **Reactive power (var)** is the part of the current that sloshes back and forth each cycle without being
  consumed: motors, transformers and the filters inside electronics cause it. It shows up as amps on the
  wire but not as watts on the bill.
- **Power factor** is real ÷ apparent, from 0 to 1. A heater or incandescent bulb is 1.00: every amp does
  work. A fridge compressor might be 0.7, a cheap LED driver or phone charger 0.5 or worse, meaning they draw
  roughly twice the current their wattage suggests.

Why care, at home: PF explains why a circuit's amps don't match its watts, which devices are stressing a
circuit more than their consumption implies, and whether a reading is trustworthy (a resistive load that
doesn't show PF ≈ 1 means the calibration is off, not the load).

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

The Vue 3 reports a current value of exactly `0x5000` on a port with no CT (verified: plugging a CT into a port
drops it to a normal reading while the others stay at `0x5000`). Those ports now publish NaN (and log once) instead of
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

### 6. Frequency / phase-angle resolution and calibration

The MCU gives the mains period as an integer count (~327 at 60 Hz on the Vue 3), so one reading resolves to
about 0.18 Hz and 1°. `frequency_samples: 25` averages 25 raw counts (~6 s) before converting, for roughly
0.01 Hz. This is why upstream readings of "60.3 Hz" look wrong: real grid frequency is held within ±0.04 Hz.

The Hz conversion is `frequency_constant / raw_period` with upstream's empirical constant (19610 on the Vue 3,
25310 on the Vue 2). Set `frequency_constant` on the BLACK phase to trim it against a meter, exactly like
voltage: `new = old × f_meter / f_vue`. Use `accuracy_decimals: 2` on the frequency sensor to see the result.

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
        angle: 180            # 180 = NA split-phase (240 V); 120 = two legs of a wye (208 V)
        voltage: { name: "L1-L2 Voltage" }
    ct_clamps:
      - { phase_id: leg_1_2, input: "4", power: { name: "Water Heater Power" } }
```

Power is computed as P<sub>a</sub> − P<sub>b</sub> from the MCU's per-leg real-power columns, which is exact
for any angle between the legs and replaces the old `multiply: 2` approximation. Voltage is |V<sub>a</sub> −
V<sub>b</sub>| by the law of cosines using the configured `angle` (default 180, giving V<sub>a</sub> +
V<sub>b</sub>).

The MCU's own phase-angle measurement is deliberately not used here: on tested hardware it reports about 45°
for two legs wired to the same phase (true value 0°), so the `phase_angle` sensor is best treated as relative,
not absolute.

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
