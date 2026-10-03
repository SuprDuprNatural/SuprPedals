# SuprPedals DSP review — 4 October 2026

The collection has a strong DSP foundation, especially its crossover phase
matching, explicit latency accounting, and the newer effects' control smoothing.
The review also found significant defects in older shared code and a real-time
ownership problem in NAM. Those have been corrected locally. The evidence below
supports specific improvements; it does not certify every setting, model, or
pedalboard as artifact-free.

**Scope and method**

Reviewed the current working tree, including the uncommitted Crush addition,
shared CleanLows processing, dry/wet changes, and Chorus mix change, against the
recent history ending at `d66f195`. Read all 19 pedals' DSP implementations, their
shared primitives, the LV2 integration paths, and NAM's engine/model integration.
For NAM Slim, traced the vendored NeuralAudio and NAM Core implementations too.

Existing work was retained. This review changes DSP and build arithmetic; it
does not change port numbers, symbols, defaults, factory presets, or the faces.
The fixes are local and have not been installed on the Pi.

Following the requested verification budget, validation used a small diagnostic
pass for concrete defects, the affected plugin builds, and the existing NAM host
check. No repository tests were added or edited. No broad regression suite,
benchmark, demo, UI inspection, or deployment was performed.

**Defects corrected**

| Priority | Finding and consequence | Change |
| --- | --- | --- |
| P1 | The shared RBJ biquad accepted cutoffs above Nyquist. A 12 kHz low-pass at a 16 kHz host rate had poles outside the unit circle and could grow without bound. | Clamp cutoff to 0.45 times the actual sample rate in both low-pass and high-pass coefficient builders. |
| P1 | Float coefficient cancellation made bass filters inaccurate at high sample rates. The 20 Hz / 192 kHz low-pass coefficients implied a DC gain of approximately 1.3327 instead of unity. | Calculate coefficients and retain filter state in double precision. Explicitly flush negligible states to avoid denormal tails. Vowel's detector now preserves that precision too. |
| P1 | NAM Slim could rebuild against a network being used for inference. Upstream compares a channel vector that its processing function replaces; it also releases the old network inside processing. | Prepare a separate model at the requested quality on the LV2 worker. Commit its internal staged network there with silent processing, then use the existing engine ownership boundary to install it. Retire the old model through the worker. Generation numbers discard superseded load results. |
| P2 | Sans restarted its 64-sample control schedule at every host call. Identical automation produced different audio at different buffer sizes. | Preserve the remaining samples in each control interval across calls, following the existing Band pattern. |
| P2 | OctavePlus clamped oscillator increments to 0.0001–0.02 cycles/sample. These bounds retuned valid notes depending on host rate and octave selection. | Permit the requested positive frequency up to a Nyquist safety bound. A 55 Hz note shifted down two octaves now produces approximately 13.75 Hz at 192 kHz, rather than 19.2 Hz. |
| P2 | Envelope and OctavePlus started with unconfigured or previously used SVF coefficients until their first control tick. Reactivation could therefore depend on the preceding preset. | Initialize the SVF from the current cutoff and resonance on the first nonempty processing call after reset. Empty calls do not consume this initialization. |
| P2 | Tuner expired pitch and decayed stale confidence once per host call. Clack uses the same detector one sample at a time, so timeout behavior depended on the caller. | Expire pitch on the sample clock and clear stale confidence deterministically. The detector remains shared; no additional analysis path was introduced. |
| P2 | Switching Band's internal two/three-band mode could leave its high-band distortion protection and compressor timing configured for the previous band boundaries. | Recompute those parameters when topology changes. This is a DSP API correction; the current LV2 face exposes three bands and does not offer this mode switch. |
| P2 | Sans's signed delay index could overflow after about 12.4 hours at 48 kHz. Fuzz's warm-up counter wrapped after about 24.9 accumulated hours of open-gate processing, and the shared tracker had another indefinitely incrementing counter. | Wrap the delay index within its ring and saturate the two duration counters once their thresholds have been reached. |
| P2 | Several production plugins used fast-math while the shared offline DSP harness used strict arithmetic. This weakened reproducibility and could invalidate finite-value checks. | Make the default Makefile flags strict and disable floating-point contraction consistently. Optimization remains at `-O3`. |

The main implementation locations are
[the shared biquad and tracker](/Users/lukethomson/AI/SuprPedals/src/OctaverDsp.h:46),
[Sans](/Users/lukethomson/AI/SuprPedals/src/SansDsp.h:495),
[OctavePlus](/Users/lukethomson/AI/SuprPedals/src/OctaverPlusDsp.h:244),
[Envelope](/Users/lukethomson/AI/SuprPedals/src/EnvFilterDsp.h:121),
[Tuner](/Users/lukethomson/AI/SuprPedals/src/TunerDsp.h:149),
[Band](/Users/lukethomson/AI/SuprPedals/src/BandDsp.h:430),
[Fuzz](/Users/lukethomson/AI/SuprPedals/src/FuzzDsp.h:336),
[NAM swapping](/Users/lukethomson/AI/SuprPedals/src/nam/SuprNam.cpp:444), and
[NAM preparation](/Users/lukethomson/AI/SuprPedals/src/nam/NamModel.cpp:87).

**Measured results**

The temporary diagnostic compared the sources as found at the beginning of this
review with the corrected sources, compiled with the same strict arithmetic.
These are local software measurements, not measurements of installed Pi binaries.

| Diagnostic | Before | After |
| --- | ---: | ---: |
| Pole radius: 12 kHz low-pass at 16 kHz | 2.4142, unstable | 0.80084, stable |
| DC gain: 20 Hz low-pass at 192 kHz | 1.33272 | 1.00000 |
| Sans: maximum sample difference between 37- and 257-frame calls with identical automation | 0.0322694 | 0 |
| Envelope: maximum sample difference after reset versus a fresh instance | 0.0151273 | 0 |
| Band: switched two-band configuration versus an instance started in that configuration | 0.483214 | 4.57 × 10⁻¹² |
| OctavePlus: 55 Hz shifted down two octaves at 192 kHz | 19.2009 Hz | 13.7492 Hz |
| Tuner: combined frequency/confidence/strobe difference across buffer partitions | 0.0153197 | 0 |

All seven diagnostic criteria were satisfied. The initial Band assertion asked
for exact zero; its remaining difference comes from the deliberate `1e-12`
input bias passing through different initial topologies. That case alone was
rerun with a `1e-9` amplitude tolerance and passed. Its residual is approximately
−227 dBFS. The other six cases were not repeated.

All 18 Makefile plugin binaries built successfully. SuprNAM and its host harness
built successfully, and the existing host check passed **24 checks with zero
failures**. That check covers LV2 controls, routing, gate, threaded latency, and
state serialization; it does not establish safety under every real Slim model
or concurrent automation pattern.

The initial build attempts stopped on environment problems: Xcode's unaccepted
license and a cached SDK path that no longer exists. The successful builds used
the installed Command Line Tools, with a process-local developer directory and
an updated local NAM CMake cache. No license was accepted and no system developer
selection was changed. No further plugin rebuilds were run after success.

Diagnostic sources, the initial-source snapshot, and logs are under
[build/dsp-review-2026-10-04](/Users/lukethomson/AI/SuprPedals/build/dsp-review-2026-10-04).
That directory is ignored by Git.

**Review across the collection**

| Effects | Assessment from source review |
| --- | --- |
| Octave and OctavePlus | The analogue-style divider and shared pitch tracker remain intact. Octave's direct path is separate from sub processing. OctavePlus retains its existing filtered/DC-blocked direct-path behavior; the corrected oscillator bounds and startup coefficients address concrete pitch and lifecycle defects. |
| Envelope and Vowel | Both use topology-preserving state-variable filters. Vowel interpolates its formant coefficients on a fixed sample cadence and normalizes band-pass peaks against resonance. Its bounded excitation, explicit nonfinite handling, and logarithmic formant motion are sound choices. |
| Compressor and Transient | The clean signal paths, sidechain filtering, soft knees/bounded gain targets, and gain smoothing are appropriate. Their attack/release and transient behavior remain voicing decisions; this pass did not recalibrate dynamics or measure new distortion figures. |
| Clack | Lookahead remains explicitly reported. Harmonic-residual subtraction, confidence/settling guards, and exact neutral reconstruction are valuable safeguards. Its shared tuner now has consistent expiration behavior. Pitch-dependent cleanup still has the documented tracking limitations. |
| Sans, Fuzz, and Forge | Retained the measured drive voicings, oversampling, and deliberate level laws. Sans/Fuzz account for their 15-sample round trip. Forge retains 4× oversampling, antiderivative clipping, protected nonlinear residue, and 23-sample latency. Existing reference-fit and alias measurements were not rerun. |
| Band | The three-band low path receives the phase compensation needed to reconstruct coherently. Clean and processed references share the crossover and oversampler. The internal mode transition now rebuilds its dependent processing settings. |
| Chorus | Retains fractional delay interpolation and the modulation-depth limit. The recent full-signal dry/wet crossfade intentionally fades the clean low band as Mix rises; full wet can remove bass below Low. This agrees with the changed metadata, but differs from the earlier always-present clean bass behavior. |
| Phase | The normalized allpass lattice is a good choice for moving coefficients. Feedback is bounded and injected with gain compensation. Coefficient interpolation and control updates are sample-counted. |
| Echo | Stationary delay taps crossfade when time changes, avoiding delay-read jumps. Ducking affects the audible return rather than feedback, and Hold has a timeout. Feedback filtering and bounds provide a sensible stability margin. |
| Space | Orthogonal Householder scattering, passive damping, and loop gains below unity provide a sound fixed-parameter FDN structure. Decay changes gains rather than delay lengths. Metallic coloration or perceived density at unusual settings remains a listening question. |
| Crush | Aliasing and quantization are intentional. Fractional capture timing, compensated drive, symmetric zero-preserving quantization, and fixed control cadence are appropriate for this effect. Adding oversampling would undermine its intended behavior. |
| VU and Tuner | VU's audio passthrough is independent of its meters. Tuner preserves the existing plugin analysis/output-port contract and the base strobe phase rate. No OLED analysis or renderer-speed behavior was changed. |
| NAM | Whole-graph deferral, timestamped output, parallel-stage joins, polarity, and explicit alignment trim remain important protections. The Slim ownership fix addresses the most serious real-time issue found in this pass. |

**Recent mixing contracts worth retaining explicitly**

Phase, Vowel, and Crush's clean-low path is a matched 250 Hz LR4 split:
`L4(input) + dryGain × H4(input) + wetGain × H4(effect)`. The separate low
reference stays at unity even when both level controls are muted. For matching
sources and gains summing to one, the crossover sum has unity magnitude and
allpass phase. It is not a phase-free wire, and unrelated wet content is not
guaranteed to sum flat.

The new return controls deliberately use half amplitude at 0 dB. Two matching
0 dB paths therefore sum to unity; one isolated path needs about +6.0206 dB for
unity. The −60 dB stored endpoint means mute. These are intentional signal
contracts, not gain errors to compensate elsewhere. Their generous positive
gain range still requires normal pedalboard headroom management.

**Limits and tradeoffs**

Double-precision shared filters and stricter compiler arithmetic can change Pi
CPU cost. No benchmark was run, so historical CPU figures are not fresh evidence
for these binaries. The filter change intentionally corrects high-rate bass
response and may slightly alter older tracking/filter output at ordinary rates.

NAM Slim now loads a replacement model, which temporarily uses additional memory
and resets that model's inference history at the swap. The old model continues
processing while its replacement is prepared. This is not a new promise of a
click-free model crossfade. Rapid Slim changes can still queue worker work; old
results cannot overwrite newer requests. Real-model stress and the Pi polarity
null check were not run. NAM still warns about model/host sample-rate mismatch
without resampling; using matching rates remains necessary for faithful timing.

The newer effects consistently validate controls and bound their wet excitation.
That policy is not uniform across every legacy processor: arbitrary nonfinite
upstream audio or malformed control values can still contaminate some older
states. Discrete voice/waveform/topology changes also do not all have dedicated
crossfades. Those are remaining hardening limits, not claims resolved by this
review. No listening, CPU, alias-floor, or exhaustive automation certification
is implied by the successful builds and focused diagnostics.
