# SuprVowel

A mono bass formant filter: morph between **OO, OH, AH, EH and EE** with
your playing dynamics, an expression pedal or a free-running LFO. Three
moving resonances give the upper harmonics a vocal shape while a complementary
low-frequency path keeps the bass foundation. No pitch detector is needed:
Throat changes the resonances, never the note being played.

## Play it

Start with **Fonky Conversation**. Play short fingerstyle notes and adjust
Sensitivity until the position dot moves usefully toward AH on louder notes.
Raise Focus for a more pronounced resonant voice; lower it for broader colour.
Release controls how quickly the mouth closes between notes. Swap From and To
for a reverse sweep.

In **Manual**, Position goes directly from From (0) to To (1). Map an
expression pedal to Position using PiPedal's existing control mapping.
Depth, Rate, Sensitivity and Release do not drive the position in this mode.

In **Envelope**, Position sets the floor and Depth adds upward travel as the
input gets louder. Motion is capped at 1. Detector attack is fixed at 5 ms;
the position and resonances have additional smoothing, so this is not a claim
of a 5 ms end-to-end sweep. Sensitivity affects the detector, not audio gain.

In **LFO**, the same floor/depth relationship is driven by a raised cosine.
It starts at the From end on activation and runs continuously, including
through silence and while another motion mode is selected. Rate is Hz;
there is no host-transport or tap-tempo synchronization in this version.

The strip above the controls shows **actual DSP position and formant centres**,
not an estimated frequency response. It clears on disconnection and renews
its subscriptions after reconnecting.

| Control | Range | Default | Purpose |
| --- | --- | --- | --- |
| From / To | OO, OH, AH, EH, EE | OO / AH | Vowel endpoints |
| Motion | Manual, Envelope, LFO | Envelope | Position source |
| Position | 0–1 | 0 | Manual position or modulation floor |
| Depth | 0–1 | 1 | Envelope/LFO travel above the floor |
| Rate | 0.05–8 Hz | 1 Hz | LFO speed |
| Sensitivity | −24 to +24 dB | +6 dB | Envelope detector gain |
| Release | 40–800 ms | 180 ms | Envelope recovery |
| Throat | −6 to +6 semitones | 0 | Shifts all three resonances together |
| Focus | 0–1 | 0.6 | Resonator Q from 2 to 10 |
| Mix | 0–1 | 0.8 | Dry to bass-protected voice |
| Level | −12 to +12 dB | 0 dB | Output trim |

This is a filter, so it needs harmonics to shape. A muted, nearly sinusoidal
note gives a subtler result than a bright pluck, a synth voice or mild drive
before Vowel. There is no hidden distortion or oscillator generating extra
harmonics. The clean low end remains audible even at full Mix.

## With SuprEnvelope

- **Envelope → Vowel**, using Envelope Companion: Vowel's LFO moves
  independently of the envelope filter. Keep some Envelope dry blend so
  the later vowel filter still has upper harmonics to work with.
- **Vowel → Envelope**: the final envelope filter gives the talking voice an
  extra wah gesture. A low cutoff can deliberately obscure the higher formants.
- For two envelope-driven stages, begin with moderate depths and different
  release times. The second pedal detects the first pedal's output; they do
  not share a sidechain.

## Factory sounds

| Preset | Behaviour |
| --- | --- |
| Fonky Conversation | Default OO → AH envelope voice |
| Yah Bass | Sharper EE → AH with faster recovery |
| Reverse Talk | AH → OO as the note grows louder |
| Slow Mouth | Slow OO → EE LFO |
| Expression Voice | Manual OO/AH midpoint; map Position to expression |
| Envelope Companion | Broader OH → EH LFO, intended for stacking |

## Signal path and engineering

The three centre frequencies use the first three bass-voice centres in the
[Csound formant tables](https://csound.com/manual/misc/formants/). Relative
levels and bandwidths are an instrument voicing, not an attempt to reproduce
human speech. Between vowels, frequencies interpolate geometrically. Throat
scales all centres by `2^(semitones/12)`, with a ceiling below Nyquist.

The resonators reuse `SvfMulti` from SuprEnvelope, whose trapezoidal
state-variable topology follows [Andrew Simper's formulation](https://www.cytomic.com/files/dsp/SvfLinearTrapOptimised.pdf).
Their bandpass outputs are peak-normalized by `1/Q`, then summed with weights
`1, 0.85, 0.35` and a fixed 1.75 voice gain. Focus changes bandwidth rather
than increasing resonant peak gain. No existing shared DSP was modified.

The envelope reuses `EnvFollower`, with a separate 30 Hz detector highpass.
The level-to-position curve is `e/(e+0.1)` after detector sensitivity. Mode
weights crossfade over 20 ms; position and ordinary controls are smoothed.
The vowel shape itself glides over 8 ms, so changing a vowel selector cannot
instantaneously replace a filter. Integrator and damping coefficients are
updated on a fixed eight-sample cadence and interpolated per sample. That
cadence never restarts at a host block boundary.

Low protection follows the explicitly voiced complementary construction used
by SuprPhase. With `H` equal to two cascaded first-order 250 Hz highpasses,
the pre-Level output is:

```
x + Mix * H(voice - x)
```

Equivalently, the wet endpoint is `(1-H)x + H*voice`. This is not an LR
crossover or a flat-magnitude promise at the crossover. Low fundamentals are
measured below; the remaining response is deliberate tone shaping. There is
no sample delay on either branch. Mix zero and Level zero are bit-exact dry,
including after a smoothed move to zero has settled.

All processing storage is fixed. There is no allocation, locking, model
loading or work queue in `run`. Nonfinite controls fall back to documented
defaults, and nonfinite input samples become zero. Resonator excitation has
an emergency ±16 bound; the dry path retains all finite float values. The
output only clamps at the float representational limit, not at 0 dBFS.
Resonances and positive Level can exceed full scale: use downstream headroom
and output trim rather than expecting an internal limiter.

## Local verification

`make test` includes `test_vowel` and `validate_vowel.py`. The latter checks
the actual built LV2 library, all 18 port indices, metadata defaults and all
six presets. With `rdflib` available it also parses every Turtle file.

Initial local results, September 2026:

- All new tests pass at 8, 44.1, 48, 96 and 192 kHz.
- Independent complex-response error below `1.6e-5` at the tested frequencies.
- Low B, E and A change by **+0.123 to +0.354 dB** at full Mix, across all five
  vowel endpoints and Throat −6/0/+6, at unity output Level.
- Automated DSP and LV2 rendering are bit-identical with irregular blocks,
  one-sample blocks in the partition sequence, and in-place processing.
- Invalid inputs, selector/control extremes, reset, reinitialization,
  silence, decay, sensitivity calibration and LFO rate/depth all pass.
- Silent automation produces exact zero; the maximum control-induced sample
  step on settled DC was `1.49e-8`. Stress tails decay to zero.
- AddressSanitizer and UndefinedBehaviorSanitizer pass.
- The entire existing `make test` gate passes; the PiPedal web build passes.
- The actual panel and display were checked in a standalone browser harness
  at 1000, 390 and 320 px in both themes. All 12 controls fit, and selector
  and keyboard interactions commit correctly. Throat uses a custom relative
  semitone unit so PiPedal does not render it as an absolute note name.
- The new display lifecycle check verifies same-instance reconnects,
  disconnect clearing, stale callbacks and unmount cleanup.
- The local staged install contains the correct binary, metadata and presets;
  source and staged binary hashes match.

The initial Mac smoke benchmark was roughly 0.23% of one local core at
48 kHz/64 frames. Hardware measurements and deployment are recorded below.

To render the actual local plugins on a mono PCM16 bass recording:

```sh
make
python3 tools/render_vowel.py input.wav build/vowel/auditions
```

The renderer writes dry, all six presets and Envelope → Vowel without
normalization. It reports peak/RMS and refuses to write clipped PCM. The
existing local bass DI rendered with no clipping: default Vowel peaked at
−8.47 dBFS and the stacked example at −7.40 dBFS.

Before Pi testing, build and stage both repositories together using the
normal backup/restore procedure in `AGENTS.md`. The complete installed
collection then contains **18 unique Supr plugin URIs**, including NAM.


## Pi deployment and live verification — 16 September 2026

Built and deployed on the 8 GB Pi 4 at 1.8 GHz, using two build jobs and the
existing Cortex-A72 NAM configuration. All ordinary-plugin tests, the NAM
metadata/graph/host checks and the matching host build completed successfully.
The host binary was already current and its hash remained unchanged.

- The Pi Vowel benchmark processed 20 seconds of audio in 0.3068 seconds:
  **1.53% of one core at 48 kHz/64 frames** in that offline run.
- The installed catalog has **18 unique Supr URIs**, including all six Vowel
  factory presets and the custom numeric Throat unit.
- All **59 plugin files**, the host binary and **126 UI files** matched their
  current source/build and staging copies by content/hash.
- The real PiPedal face rendered all 12 controls with working DSP telemetry.
  A browser disconnect/reconnect resumed the display correctly.
- A temporary enabled Vowel instance at zero Mix retained the dry signal while
  exercising LFO/filter processing. The reported position spanned approximately
  0.00390–0.99610. There was **zero xrun increase** over the measured steady-state
  interval, with the complete test board reporting about 17.25% audio CPU.
  The live device ran at 44.1 kHz, 64-frame periods, four periods.
- The original unsaved board was restored exactly after both restart and live
  testing: all eight items, 85 controls, paths, snapshots and board metadata.
  Preset selection and dirty state were also unchanged. No test instance was left.

The first install attempt rolled back because its web health check ran before
PiPedal's listener opened. The successful retry waited for web readiness;
LV2 staging was also moved outside the bundle search path. Future deployment
scripts must account for the gap between systemd `active` and HTTP readiness.

The full installed software/data and both source/build trees were backed up
before deployment. Live-board backups and verification artifacts are under
ignored `build/vowel-deploy-20260916/`; the Pi backup is
`~/supr-vowel-backup-20260916/`.

Physical expression-pedal operation and the player's listening evaluation of
the new voice remain to be tried. Start with Fonky Conversation, then compare
Envelope Companion with SuprEnvelope before Vowel.
