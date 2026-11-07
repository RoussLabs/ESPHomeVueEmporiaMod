# Notes

Catch-all for things learned along the way that don't belong in the README's configuration sections.

## Home Assistant recorder and the Vue's data rate

### How much data

The example config publishes about 55 sensors (power, PF and current per CT, mains, voltages, frequency,
energy) every 5 s. The recorder stores every state change in the `states` table and keeps it for 10 days by
default (`purge_keep_days`), so that is roughly 700,000 rows a day and about 7 million rows resident at steady
state. Long-term statistics (hourly mean/min/max, and the totals the Energy dashboard uses) are computed
separately, are tiny, and are never purged, so the raw-state volume affects only the detailed 10-day history
views and the recorder's own write queue.

### Is it a problem?

**Drive wear: no, on an SSD.** A 1 TB consumer NVMe drive is rated around 600 TBW
([Samsung 990 EVO spec](https://techpowerup.com/ssd-specs/samsung-990-evo-1-tb.d1868)). Even a pessimistic
few GB/day from the recorder is on the order of 1 TB/year: hundreds of years of rated endurance. The
corruption stories in the HA forums are almost all SD-card installs, which have no endurance rating and no
wear-levelling controller
([SD vs SSD write-up](https://evezone.evetech.co.za/deep-dives/sd-card-vs-ssd-for-home-assistant-in-2026-why-your-pi-keeps-corrupting)).

**Query performance: not demonstrated on NVMe-class hardware.** The threads with hard numbers are Pi + SD, or
multi-gigabyte installs with thousands of entities:
- [Running with very large databases](https://community.home-assistant.io/t/running-with-very-large-databases/953475):
  80 GB MySQL, 10–26 GB MariaDB; the one clear finding is that a 26 GB MariaDB "runs fine on modest hardware
  with fast storage and adequate RAM".
- [Charts loading is slow](https://community.home-assistant.io/t/charts-loading-is-slow-is-it-worth-migrating-recorder-to-postgresql/976027):
  NUC12 + enterprise NVMe, 1.3 GB SQLite, history dashboards "a few seconds"; replies could not attribute it
  to the database and suspected the volume of points sent to the browser. A 3,500-entity tuned-MariaDB user
  reported a 43-graph dashboard loading in under a second.
- [Speed up HA by reducing database size](https://community.home-assistant.io/t/speed-up-home-assistant-by-reducing-the-database-size/175094)
  and [Taming the Recorder](https://community.home-assistant.io/t/taming-the-recorder/271932): the classic
  advice, written for Pi/SD systems; no measurements on capable hardware.
- [Long-term statistics](https://www.home-assistant.io/docs/configuration/long-term-statistics/): official
  description of the 10-day state history, 5-minute/hourly statistics, and what happens when an entity is
  excluded from the recorder (existing statistics stay; no new ones are generated).

Decision for this install (x86, NVMe, SQLite): run everything at a uniform 5 s, monitor, and change only if a
measured problem appears. Uniform rates matter for analysis; mixing 5 s and 60 s sensors makes graphs
deceptive and troubleshooting slow.

### If it does become a problem

All of these target only the Vue and are reversible:
1. Exclude the diagnostics from the recorder; live values and automations are unaffected:
   ```yaml
   recorder:
     exclude:
       entity_globs:
         - sensor.*_power_factor
   ```
2. Move the recorder to the MariaDB add-on (one `db_url` line, a few hours of data copy).
3. Reduce `purge_keep_days` (global, so least attractive).
4. Feed a time-series database (InfluxDB / VictoriaMetrics) for long-range full-resolution analysis.

### Monitoring the database

Three sensors and one log filter tell you whether the recorder is coping. Create the sensors with the SQL
integration: **Settings → Devices & services → Add integration → SQL**, leave *Database URL* empty for the
default SQLite database, paste the query, set the column name, unit and a name. Each one runs every 30 s by
default; the heavier queries below are better set to a longer interval or created on demand.

**1. `HA Database Size`** — size of the SQLite file in MB. Rises for `purge_keep_days` days after a change,
then flattens; the flat value is the steady state. A line that never flattens means purging is not keeping up.
```sql
SELECT ROUND(page_count * page_size / 1024.0 / 1024.0, 1) AS size
FROM pragma_page_count(), pragma_page_size();
```
Column `size`, unit `MB`.

**2. `HA States Rows`** — number of rows in the `states` table. The direct measure of what the history views
have to scan. At 5 s × 55 sensors expect ~7 million at steady state.
```sql
SELECT COUNT(*) AS rows FROM states;
```
Column `rows`. (Full count; on a large table this takes a second or two, so give it a 10-minute interval.)

**3. `HA Top Recorder Entity`** — which entity is writing the most rows, with its count. Run on demand rather
than as a sensor if you only want it occasionally.
```sql
SELECT m.entity_id || ' (' || COUNT(*) || ')' AS top
FROM states s JOIN states_meta m ON s.metadata_id = m.metadata_id
GROUP BY m.entity_id ORDER BY COUNT(*) DESC LIMIT 1;
```
Column `top`.

**4. Recorder backlog warning** — the real failure mode is the write queue falling behind, not size. HA logs
it; make sure the message is visible:
```yaml
logger:
  default: warning
  logs:
    homeassistant.components.recorder: warning
```
Look for `The recorder queue reached the maximum size` or `Recorder is backed up`. If those never appear, the
writer is keeping up.

**5. The number that matters: history page load time.** Open History for ~10 Vue entities over 24 h now (empty
database) and again after 10 days at steady state, and note both. That is the user-facing cost; the forum
threads disagree because nobody measured it.

## Findings from testing (Vue 3, Oct–Nov 2026)

- Checksum byte is CRC-8, poly 0x07, init 0x1D, over bytes 2–283. Verified on every packet since.
- Empty CT port reports current `0x5000`, not `0xFFFF`.
- Branch inputs 1–16 read inverted relative to mains A/B/C (`invert: true`).
- Upstream factors hold: 22 (branch) and 5.5 (mains) within 0.5 % of a Fluke 87V; current scalars within
  0.5 %; voltage calibration 0.01922 / 0.01923 on this unit vs upstream's 0.01925.
- Frequency constant 19623 (upstream 19610) against the Fluke's 60.01 Hz.
- MCU phase-angle field reads a correct 179° in a split-phase panel; on a bench with both inputs on one phase
  it reads a meaningless 40–65°.
- Three failure signatures seen while wiring 16 CTs: NaN = CT not seated; 0 W with current present = wrong
  `phase_id` (negative hidden by `*pos`); a fraction of the expected load = clamp not snapped shut or on the
  wrong conductor. A `power_factor` sensor on every CT makes the second one visible instead of silent.
