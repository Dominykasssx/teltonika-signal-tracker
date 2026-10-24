# Signal optimization algorithm

The scan runs inside the `signal-tracker` daemon on the router. It is event-driven:
moves, settling delays and measurements are asynchronous, so the WebUI, ubus and other
RUTOS services are never blocked.

## Steps

1. **Start** (WebUI *Start Scan* → `ubus call signal_tracker scan_start`). The saved
   configuration is reloaded. The scan is refused if the controller is disconnected, a
   move is in progress, a scan is already running, the configuration is invalid, or
   the configured range exceeds the ESP32's limits.
2. **Baseline** (only if the start position is known, i.e. the last move completed and
   the servo is still driven): the current angle is measured without moving.
3. **Coarse sweep**: `min_angle, min_angle + coarse_step, ...`, plus `max_angle`. Angles
   already measured are skipped. The sweep starts from the end closer to the servo.
4. **Fine sweep** around the best coarse angle `c`: every `fine_step` within
   `c ± (coarse_step − fine_step)`, clipped to the range, skipping measured angles.
5. **Return**: move to the best measured angle. With `min_improvement > 0` and a
   baseline, the servo returns to the starting angle instead unless the best angle
   beats it by at least that many score points.
6. **Result**: final angle, best and baseline measurements, every measured angle, and
   a flag saying whether the scan was complete.

## Measuring one angle

1. Send `set_position`, then poll `get_status` every 200 ms until the controller
   reports the move finished. Exceeding `move_timeout_ms` fails the scan.
2. Wait `settle_ms` so the modem reports values for the new orientation.
3. Take `samples` measurements, `sample_interval_ms` apart. Each runs
   `gsmctl [-O <modem>] -q` (RSSI/RSRP/RSRQ/SINR) and then `gsmctl -C` (serving cell id)
   asynchronously.
4. Aggregate the samples into a score (below).

## Scoring

For each angle:

1. **Validity**: a sample is valid if gsmctl succeeded and reported at least one of RSRP,
   RSRQ, SINR. Values outside the 3GPP reporting ranges (RSRP −156..−31 dBm, RSRQ
   −34..3 dB, SINR −23..40 dB) are treated as **missing, never as zero**.
2. **Serving cell**: if samples report different cell ids, only samples from the most
   frequent cell are kept, and the angle is flagged `cell_changed`.
3. **Median** of each metric over the kept samples. A metric counts only if at least
   `min_valid_samples` samples report it. Medians resist single outliers, which favours
   a stable connection over one lucky reading.
4. **Normalise** each median linearly to 0..1 (clamped):
   RSRP −140..−44 dBm, RSRQ −20..−3 dB, SINR −10..30 dB.
5. **Weighted mean** over the metrics that are available:

   `score = 100 × Σ wᵢ·normᵢ / Σ wᵢ` (only metrics present at this angle)

   Default weights are RSRP 0.5, RSRQ 0.2, SINR 0.3. A missing metric drops out and the
   weights are renormalised. A modem without SINR is therefore scored on RSRP and RSRQ.
6. An angle with fewer than `min_valid_samples` valid samples, or with no weighted metric
   available, is **not scored**. Three unscored angles in a row stop the scan
   ("no valid signal measurements").

**Best angle**: the highest score. Ties go to the higher RSRP, then to the angle measured
first.

Caveat: if some angles have SINR and others don't, their scores are built from
different metric sets. The results table shows which metrics each angle had.

## Completion and honesty of results

- `complete = true` only after every planned angle was measured and the return move
  finished. The UI then says *best measured angle*. Even a complete scan only covers the
  scanned grid: it is not a guaranteed global optimum, and the fine sweep refines only
  around the best coarse angle.
- **Cancel** stops the scan at once. A move already sent finishes, but no new move is
  started and the servo is **not** moved to the best angle. Partial results are kept.
- **Failure** (USB unplugged, controller reboot, timeouts, no valid measurements) stops
  the scan without further movement and keeps partial results. The UI marks the best
  angle as *best of the angles measured so far*.
- **Serving-cell changes between angles** are reported, because scores from different
  cells are being compared.
- **Stale values**: the daemon can't tell whether the modem refreshed a value
  between two reads (gsmctl gives no timestamp). Freshness relies on `settle_ms` and
  `sample_interval_ms`; increase them if values look frozen.

## Duration

Per angle ≈ move time + `settle_ms` + (`samples` − 1) × `sample_interval_ms` + gsmctl time.
With the defaults (4 s settle, 5 samples, 2 s apart) that is about 12–13 s per angle.
A 0–180° scan measures 19 coarse and about 8 fine angles, so roughly **6 minutes**.

## Simulation mode

`option simulate '1'` (and a service restart) replaces the ESP32 with an emulated
controller that speaks the same protocol, and the modem with a synthetic signal field.
The field has a main lobe at `sim_best_angle` and a weaker side lobe 90° away. The host
tests run the full scan against it with a known best angle; with zero noise the scan
must return exactly that angle.
