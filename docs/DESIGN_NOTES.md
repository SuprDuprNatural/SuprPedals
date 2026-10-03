# DSP design notes

This document records the decisions that are easy to break by accident. The
numbers come from the offline suite, capture probes or real bass recordings.
Run `make test` before and after changing a DSP path.

## Shared rules

### Keep the audio callback bounded

The seventeen regular pedals use fixed storage and allocate nothing while
processing. Their DSP is header-only so the same implementation runs in the
LV2 plugin and the offline harness. SuprNAM loads models outside the audio
thread and uses pre-created real-time workers when Threaded mode is enabled.

### Keep parallel paths aligned

A clean blend is only useful when dry and processed signals arrive together.
SuprSans, SuprFuzz and SuprBand use one 2× halfband round trip with a fixed
15-sample delay. Band also matches dry/wet crossover phase before blending.
SuprNAM either runs its whole wet graph inline or delays the whole graph by
the negotiated maximum block length; it never delays
only one branch.

### Make neutral settings a real identity

Where the topology permits it, disabled sections contribute an exact zero and
neutral controls are bit-exact. Tests cover this directly. A few deliberate
exceptions are documented:

- SuprClack always carries its reported 2 ms lookahead.
- SuprChorus uses an LR4 crossover. Its bands sum magnitude-flat through an
  all-pass phase response, so Mix 0 is not bit-exact.
- Sans, Fuzz and Band carry a 15-sample round trip; Forge carries 23 samples.
- Band Blend=0 retains the matched crossover allpass phase.

### Let the DSP publish display state

Meters and custom browser displays use scalar output ports from the plugin.
Fast events are peak-held long enough for a 30 Hz monitor to see them. Do not
recalculate compressor gain, filter position, tuner state or reduction in the
renderer.

## Pitch and pitch-driven pedals

### SuprOctave and SuprOctavePlus

The octave voice follows the analogue OC-2 pattern: a filtered input clocks a
flip-flop, and the divided polarity multiplies a band-limited copy of the
input. This keeps the player's dynamics instead of replacing them with a
generated oscillator.

SuprOctave adds a symmetric soft drive at 2x sample rate before its output
filters. Drive spans 24 dB of extra gain with 9 dB of gradual output trim.
The high-pass (Off, then 10–1000 Hz) and low-pass (150–4000 Hz) are 12 dB/octave
TPT sections, with smoothed cutoffs. They shape only the generated voice;
the direct path and pitch detector stay clean. The panel is three rows:
Direct / Oct 1, Drive / Gate, High pass / Low pass. The existing `tone` port
remains the low-pass control; Drive and High pass are appended ports.

The tracker uses:

- a four-pole acquisition low-pass which settles near `1.35 × f0` after lock;
- envelope-scaled comparator hysteresis;
- a pitch-dependent refractory period;
- separate handling for attacks, note changes and suspicious period jumps;
- a smooth envelope gate for decaying notes.

On the test material, note changes lock in roughly 35–120 ms. The same tracker
drives OctavePlus. Its two polyBLEP oscillators are pitch-locked to the string,
the filter envelope retriggers from detected attacks, and the amplitude
envelope remains the played string's envelope.

### SuprTuner

The tuner low-passes and decimates the analysis copy to about 6 kHz, then uses
a McLeod-style normalised square difference function. It chooses the earliest
strong peak rather than the tallest one; that prevents a strong second
harmonic from becoming an octave error on bass.

Key settings are a 1152-sample analysis window, 96-sample hop, 384-sample
maximum lag and parabolic peak interpolation. A lock is held for about 280 ms
without a valid update to stop display flicker. Across 44.1, 48 and 96 kHz and
host blocks from 1 to 257 samples, the pure-tone test stays within 0.307 cent;
low B stays within 0.04 cent.

The audio output is a copy of the input unless Mute is active. Mute uses a
short fade. The plugin publishes `frequency`, `note`, `cents`, `confidence`,
`level` and wrapped `strobe` phase.

## Dynamics

### SuprCompressor

This is a feed-forward compressor with a soft-knee gain computer and
attack/release smoothing in the dB domain. The knee widens at low ratios and
tightens towards limiting ratios. A fourth-order sidechain high-pass stops low
fundamentals from controlling the whole signal. Makeup and parallel blend are
smoothed, and gain reduction is published as a meter port. The
main audio path is unfiltered while DC protection remains in the detector.

### SuprTransient

The attack response was fitted point-by-point to a measured SPL Transient
Designer. Its attack law is intentionally level-dependent: hard isolated hits
receive more boost than soft hits, dense runs receive little, and a rest or
mute re-arms the detector. A purely relative detector tested cleanly but did
not produce the same behaviour.

The important implementation choices are:

- **HR** offsets detector readings only. It calibrates the level-dependent
  curve without changing audio gain. Neutral settings remain bit-exact at any
  HR value.
- **Focus** applies attack gain only above a complementary one-pole split. The
  algebra collapses the split when both gains match, preserving the neutral
  identity.
- A 20 ms peak hold prevents low-frequency waveform ripple from modulating the
  gain. The 40 Hz regression moved from about −25 dB THD to −151.7 dB.
- There is no lookahead. Negative Attack changes the attack envelope but is
  not a limiter and will not reliably reduce the first peak of a pluck.

At Focus 400 Hz, a test burst moved a 60 Hz component by 0.19 dB instead of
the 4.9 dB full-range result.

### SuprClack

SuprClack combines a high-frequency transient duck, a pitch-locked harmonic
sieve, a between-note squeak duck and a low-band-keyed hard gate. It uses 2 ms
of lookahead so the gain is already down when a 1–5 ms click arrives. Its
algorithm, safety guards and known limitations are documented in
[SUPRCLACK.md](SUPRCLACK.md).

## Measured drive pedals

### SuprSans

SuprSans was fitted to NAM captures of a Tech 21 SansAmp Bass Driver. Offline
inference reproduced the reference capture at 0.9979 normalised correlation,
which made it suitable for measurement.

The captures established that:

- the main nonlinearity is symmetric; odd harmonics dominate even harmonics;
- the curve is close to `x / (1 + |x|)` and is never completely clean;
- the voicing includes a sharp notch near 3.6 kHz, not a cabinet-style hard
  low-pass;
- the fitted response is within 0.3 dB RMS from 25 Hz to 12 kHz;
- drive must be calibrated from harmonic content in loud frames, not from
  crest factor or a whole-file average.

Low frequencies are reduced before saturation and restored afterwards so the
fundamental does not dominate the nonlinearity. High frequencies receive the
opposite pre/de-emphasis. The nonlinear band is limited and the clean top is
added around it, avoiding aliased hash while retaining the reference unit's
open high end. The clean blend uses the same 15-sample delay as the wet path.

At maximum drive the plugin is slightly more compressed than the reference:
roughly 11 dB crest factor versus 13.3 dB.

### SuprFuzz

SuprFuzz was fitted to two Big Muff for Bass captures at different Sustain
positions. The pedal remains strongly distorted from −60 dBFS to 0 dBFS, with
similar harmonic ratios at each level. A single clipper cannot reproduce that.

Two cascaded stages fit all 36 reference harmonic measurements to 1.07 dB RMS.
The fitted curve is

```text
x / (1 + |x|^(1/3))^3
```

with heavy first-stage gain. The second stage receives a small bias to match
the measured even harmonics. The bass version's voicing is close to flat below
5 kHz; the familiar guitar-Muff mid scoop was not present in the captures.

Because the cascade is level-independent, a fixed output trim only matches one
playing level. The shipped output matcher compares long-term wet and delayed
dry power over 2.5 seconds, updates only while the gate is open, and warms up
quickly after engagement. It holds the level match within about 0.6 dB from
full input down to −24 dB without removing the fuzz's within-note sustain.

The gate stays open while pitch is still tracked, and the clean blend uses the
same fixed 15-sample delay as the wet path. Known residuals are 2–3 dB less
energy than the capture from 1–8 kHz and more transient compression than the
reference.

## Crossovers and modulation

### SuprBand

Two LR4 bands reconstruct flat, but a three-band tree does not unless the low
band also receives the phase of the second crossover. Without correction the
lower split can dip by 3.56 dB when the splits are one octave apart and nearly
12 dB when they are close. Passing the low band through a second-order all-pass
at the upper split makes the full sum `AP2(f1) × AP2(f2)`. The suite measures
0.000 dB error even with close or crossed split controls.

All three bands run inside one oversampler. Per-band oversamplers would create
three opportunities for delay mismatch. A driven band adds only the
high-passed *difference* between its clean and saturated versions; this keeps
intermodulation out of the octave below it and preserves the crossover
reconstruction. The worst measured low-band change across the drive range fell
from 5.4 dB to 0.11 dB.

Blend uses the clean reference within the same crossover/oversampling path as
wet. Neutral intermediate blends remain flat; Blend=0 includes crossover phase.
An integer dry delay alone cannot provide this match.

Compressor timing is derived from each band's frequency: roughly two periods
for attack and twenty for release, with practical limits. This avoids an
attack setting that follows the waveform instead of the envelope.

### SuprChorus

A complementary `high = input - lowpass(input)` split is unsuitable here.
Low-pass phase rotation puts a large fraction of the fundamental into that
nominal high band even when its magnitude response looks safe. The chorus uses
an LR4 crossover instead. Low filters the wet voices; Mix crossfades the
complete dry signal, including the low band, to those voices. Full Mix
contains no clean bass. With the split disabled, the whole audible band choruses.

Delay slope determines pitch deviation, so Rate and Depth multiply. Depth is
therefore a maximum and the DSP caps peak detune near 50 cents at faster rates.
Up to three Catmull–Rom-interpolated taps share one delay line.

### Clean lows in Phase, Crush and Vowel

These effects share `CleanLowsDsp`: a 250 Hz LR4 split with an independent,
unity-gain low input band. Dry and Wet gains apply after filtering only their
upper bands, so neither controls the low reference or its filter history.
Matched low/high phase avoids the residual-crossover bass leakage described
above. L4+H4 reconstructs a unity-magnitude allpass, with no sample buffering;
it is not a phase-free bypass. See [Linkwitz's crossover derivation](https://www.linkwitzlab.com/crossovers.htm).
Both levels muted retain clean lows; turn Clean lows off for full-band mixing.

## SuprNAM

NAM models are causal: receptive field is history, not output delay. Parallel
models remain coherent if the host does not introduce different scheduling
delays. The Threaded engine therefore hands off the complete wet graph and
returns timestamped samples at a fixed maximum-block delay, including with
irregular host blocks. Mode switches are asynchronous and crossfaded. Parallel stages may fan out across helper
cores, but every branch joins before the stage is mixed. Tests cover all seven
routings and exact cancellation of two identical models with one polarity
inverted.

Other important choices:

- A 5 Hz DC blocker runs on every model path before mixing. Asymmetric models
  otherwise add offsets that move when Blend changes.
- Linear is the default blend law because related model outputs are correlated.
  Equal-power remains available for dissimilar sources.
- Each path has polarity and 0–4 ms sub-sample alignment. Different models can
  have genuinely different phase responses; the plugin exposes that rather
  than trying to hide it.
- Missing `input_level_dbu` metadata falls back to unity. Treating
  NeuralAudio's substituted +12 dBu default as real made level spread worse.
- `suprnam.so` exports only `lv2_descriptor`. Hiding the statically linked NAM
  symbols prevents interposition with other NAM plugins in the same host.
- The NAM target omits `-ffast-math` so reassociation cannot break exact
  cancellation between parallel branches.

A previously measured large model used about 46% of one Pi 4 core at 1.8 GHz;
actual cost depends on the capture and routing. One inline model is
comfortable; two or three should normally use Threaded mode.

## Verification

SuprVowel's design and measured acceptance criteria are in
[SUPRVOWEL.md](SUPRVOWEL.md). It reuses SuprEnvelope's SVF/envelope primitives
without changing them; pitch tracking is not part of its signal path.

```sh
make test       # regular pedal suite
make nam-test   # NAM graph, bundle and optional real-model checks
make tools      # capture-measurement probe
```

For a real-hardware NAM check, load the same model into A and B, choose `A∥B`,
invert B and listen for silence. Repeat with Threaded enabled. Anything audible
means alignment, trim, routing or the deployed build is wrong.
