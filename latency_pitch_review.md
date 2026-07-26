# Latency & Pitch-Stability Work Plan (reviewed + revised)

Date: 2026-07-13 (revised after review discussion)
Scope: `backends/parallel_hybrid_engine.hpp`, `backends/predictive_pitch_tracker.hpp`,
`harmonizer_web.cpp` (output bridge, audio routing), test scripts.
Goals: reduce latency, improve pitch stability
(per [bloomberg_thesis_notes.md](bloomberg_thesis_notes.md) Priority 1).

Guiding rule for everything below: **measure before tuning**. Telemetry and
profiling come first; parameter changes are hypotheses to be arbitrated by the
render harness, not installed by assumption.

## Implementation disposition (2026-07-13)

| Review item | Result |
| --- | --- |
| Bridge clocks | Implemented PI-controlled fractional resampling, four-point interpolation, occupancy/rate telemetry, and symmetric 3 ms recovery fades. Ten-minute `+/-100 ppm` simulations pass. |
| Callback burst | Callbacks now only copy samples. A wake-driven macOS time-constraint DSP worker and four persistent quality lanes reduced the live 16-voice worst batch from about 16 ms to 3.47 ms; the 15-second stress run had zero xruns and a bounded 448-frame queue. |
| Flutter compensation | Disabled in `parallel`. Across 2/4/6 Hz, two depths, and three target intervals, compensation increased mean SD from 0.146 to 0.166 semitone. The `live512` reference is unchanged. |
| R2 timestamp | A bracketed render sweep selected `-1184` samples relative to the old target; nearby `-1120..-1248` values form the same low-error basin. |
| Predictor gains | Added deterministic noise, outlier, glissando, step, dropout, and multi-rate fixtures. Kept gains `0.60/0.30` to preserve the real 22 ms projection win, but capped velocity innovation at 0.12 semitone; outlier peak fell from 0.581 to 0.368 semitone. |
| Smaller fixes | Confidence is smoothed over 5 ms; bridge loss/recovery is faded; DSP and PortAudio device latency are labeled separately; device menus refresh; fixture analyzer builds at `-O3`. Physical cable loopback and MacCore mode A/B remain measured follow-ups, not assumed changes. |

---

## 1. Bridge occupancy telemetry, then clock-drift control — highest priority

**Where:** `harmonizer_web.cpp` — bridge write/read in the audio callbacks
(~line 625 onward); bridge constants in both engine headers.

**Problem (agreed):** separate input/output PortAudio streams on separate
hardware clocks with no rate reconciliation. Occupancy either creeps (mic
clock faster; occupancy = added output latency, pinning at +186 ms with input
drops when the ring saturates) or drains (output clock faster; unprime →
silence → re-prime dropout, repeating).

**Correction cadence:** at 20–100 ppm the streams diverge by ~0.9–4.4
samples/s, so a one-sample correction is needed roughly every **0.23–1.13 s**
— not "every few seconds". Any slip-based corrector must run at that cadence.

**Plan, in order:**
1. **Telemetry first:** track bridge occupancy (instantaneous + slow EMA) and
   expose it in `stateJson`/the GUI meter. Confirms the drift direction and
   magnitude on the real rig before any control is added, and note that normal
   occupancy is *not* pinned at the 256-frame prime — input starts ~8 ms
   before output, so measure what steady-state actually is.
2. **Preferred fix:** PI-controlled fractional resampler on the bridge read
   side, servoing occupancy to the measured steady-state target.
3. **Acceptable MVP:** crossfaded one-sample slip (drop/duplicate with a short
   crossfade) driven by the same PI error term, at the cadence above.

**Payoff:** constant, measurable latency; the periodic dropout mode disappears;
the "zero xruns over a rehearsal" criterion becomes attainable.

---

## 2. Profile the quality-path CPU burst, then stagger

**Where:** `parallel_hybrid_engine.hpp` — `processQualityBlock()` (~line 573):
16 × LiveShifter(512) + voicing/sibilance analysis run in one burst inside a
64-frame (1.45 ms) input callback every fourth 128-block.

**Diagnosis agreed; payoff estimate revised.** The prime cushion absorbs the
burst, but shrinking the prime 256 → 128 does **not** automatically save
2.9 ms — steady-state occupancy is set by stream start timing (see item 1
telemetry), not by the prime constant alone. Measure first.

**Plan, in order:**
1. **Measure worst-case callback time** (per-callback duration histogram,
   1/4/8/16 voices, on the slowest target machine). Add to telemetry.
2. **Stagger quality voices** across the four 128-blocks of each 512 window
   (voice `v` runs when `blockIndex % 4 == v % 4`, per-voice input phase
   offsets) so CPU is flat rather than bursty.
3. **Re-measure**, then lower bridge latency to whatever the flattened worst
   case actually supports.
4. **Worker thread** (callbacks only copy; DSP on a dedicated RT thread) is an
   isolation improvement, not a capacity fix — it cannot help if average DSP
   cost exceeds real time. Consider after 1–3.

**MacCore mode:** `paMacCorePlayNice` vs `paMacCorePro` changes device
parameters (not literally hog mode). Worth an A/B *after* drift control makes
latency measurable; persist the choice as a setting.

---

## 3. Flutter compensation: multi-rate A/B, then decide

**Where:** `parallel_hybrid_engine.hpp` — constants ~199–203, application
~602–616, `updateFlutterState()` ~551–570.

**Critique (agreed):** the squared-delta terms are sign-blind and primarily
generate a 2f component; the first-order residual of lag-delayed control on
vibrato is at 1f and sign-dependent, so the term cannot cancel it in general.
The 5.3/−1.6 constants look tuned against the single synthetic 2 Hz vibrato in
`test_parallel_handoff.sh`.

**Test:** render the handoff fixture at 2, 4, and 6 Hz vibrato and at other
depths/intervals from the A/B matrix, compensation on vs. off. If it only wins
at 2 Hz, remove it.

**Replacement (NOT to be installed directly):** `slope × lag` is sign-aware in
principle, but forward-projecting Live512 control has already performed worse
in practice because LiveShifter's control acts on buffered audio
([bloomberg_thesis_notes.md line 306](bloomberg_thesis_notes.md)). Before any
replacement: **measure the effective control-to-audio offset** of the quality
path (e.g., step the pitch scale on a steady tone and locate the response in
the render), then decide what lag term, if any, is correct.

---

## 4. R2 control timestamp: empirical sweep, no assumed offset

**Where:** `parallel_hybrid_engine.hpp` — `projectedPitchForBlock()`
(~line 399), currently targeting `now − blockSize/2`.

**Revision of the original proposal:** subtracting `earlyStartDelaySamples`
by assumption is not justified — `getStartDelay()` is an output alignment
delay, not evidence that a pitch-scale change applies to audio from 512
samples earlier. The pitch scale is set immediately before processing the
current 128-sample block, which makes `now − blockSize/2` a defensible
default.

**Plan:** sweep the projection timestamp offset (e.g., 0, −128, −256, −384,
−512 samples relative to the current target) through the handoff render and
pick the offset that minimizes the vibrato-amplitude and sd gates. Same
measure-the-offset philosophy as item 3.

---

## 5. Predictor gains: expand fixtures, tune jointly

**Where:** `predictive_pitch_tracker.hpp` — `kPositionGain = 0.60`,
`kVelocityGain = 0.30` (applied as `slope += gain · residual/dt`,
dt ≈ 11.6 ms).

**Problem (agreed):** a 0.5 st glitch below the 1.25 st jump-reset slams the
slope ~13 st/s; projected over up to 55 ms that is ~0.7 st of transient error
on the early path. The clean-sinusoid test cannot catch this.

**Plan:**
1. Extend `tests/parallel_pitch_tracker_test.cpp` with a fixture matrix:
   deterministic measurement noise, isolated outliers, glissandi, note steps,
   dropouts, and 2–6 Hz vibrato at multiple depths.
2. Tune **position and velocity gains jointly** against that matrix. The
   earlier 0.08–0.15 velocity-gain suggestion is a hypothesis only; let the
   matrix decide (possibly alongside a one-pole on the slope state).

---

## 6. Smaller fixes (agreed, low risk)

- **Smooth the confidence gain** (`parallel_hybrid_engine.hpp` ~685–687):
  `provisionalGain = predictorConfidence` steps 0.35 → 0.75 → 1.0 at hop
  boundaries during onsets; one-pole it per-sample.
- **Fade underflow exit *and* recovery** (`harmonizer_web.cpp` ~684–687): both
  the cut to silence and the re-primed re-entry should ramp 2–3 ms.
- **Report physical/device latency separately** from the DSP-path figure:
  label `earlyPathLatencyMs`/`qualityPathLatencyMs` as DSP-path in the GUI;
  automate a true loopback measurement with the test tone + capture.
- **Refresh device lists after hotplug** (`harmonizer_web.cpp` ~892–893)
  instead of enumerate-once-at-startup.
- **`pitch_analyzer` at `-O0`** (Makefile line 4): harmless to live audio;
  bump for faster fixture runs when convenient.

---

## Agreed order of work

1. Bridge occupancy telemetry and drift control (item 1)
2. Callback profiling (item 2, steps 1–2)
3. Expanded pitch fixtures (item 5)
4. Re-evaluate flutter compensation against the multi-rate matrix (item 3)
5. Confidence-gain smoothing (item 6)
6. Empirical R2 timestamp sweep (item 4)
7. Lower bridge latency and test MacCore modes (item 2 step 3 + mode A/B)

All work stays inside the `parallel` backend and bridge plumbing; the
`live512` reference remains untouched, per the design-decisions list in
[bloomberg_thesis_notes.md](bloomberg_thesis_notes.md).
