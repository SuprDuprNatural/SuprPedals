# SuprPhase

Dry and Wet levels use dB relative to a 50/50 blend: **0 dB on both gives
unity for matching signals**. Both knobs centre at 0 dB, reach +24 dB and
mute at −∞. The mute endpoint is stored as −60 in LV2. They use the same
continuous knob and text readout as Vowel's Throat control.

Mono bass phaser with LFO or envelope movement and optional clean-low
protection. Four normalized first-order lattice allpasses create two deliberate
moving notches. This is phase modulation, not a delay or stereo widener.

| Control | Behaviour |
| --- | --- |
| Rate | Free-running sine LFO, 0.03–5 Hz. No host transport synchronization. |
| Depth | 0–1; full depth sweeps ±2 octaves about Centre. Zero holds Centre. |
| Centre | 100–2500 Hz; swept centre clamps to 25 Hz–min(10 kHz, 0.22 × sample rate). |
| Feedback | −0.75 to +0.75. Injection scales by 1−abs(Feedback), deliberately controlling resonant gain. |
| Dry Level | Independent dry upper-band gain with Clean lows on, −∞ to +24 dB, default 0 dB. |
| Wet Level | Independent phased return gain, −∞ to +24 dB, default 0 dB; controls only the upper band with Clean lows on. 0 dB gives half amplitude; −∞ mutes. |
| Clean lows | Default On. Keeps a separate unity-gain clean low band below 250 Hz, independent of both levels. Compact button colour shows state. |
| Mode | LFO or Envelope, with a smoothed crossfade between their movement signals. |
| Sensitivity | −24 to +24 dB envelope detector gain; independent of audio gain. |

Envelope mode uses a rectified input follower with 8 ms attack and 180 ms
release. Its normalized movement is `2e/(e+0.1)−1`, where `e` includes
Sensitivity. Quiet input returns toward the low sweep endpoint; louder notes
open the sweep. Sensitivity gives soft playing useful travel. Rate continues
running internally in envelope mode, so changing modes does not restart it.
Activation resets the LFO to phase zero and clears all state.

## Return levels, lows and stability

With protection off, output is `dryGain × input + wetGain × effect`. Positive wet
polarity is intentional. At zero feedback, the allpass branch has unity
magnitude, while its phase creates two cancellations at a half blend. For
Centre 700 Hz at 48 kHz the notches are 290.12 and 1684.28 Hz; measured rejection
exceeds 90 dB. Feedback includes a one-sample loop delay, accounted for in
the complex-response tests. It changes both resonance and notch depth.

With Clean lows on, a matched 250 Hz Linkwitz–Riley fourth-order crossover
routes the input low band directly to the output at unity gain. Dry and Wet
Level apply only to their high bands:

```
L4(input) + dryGain * H4(input) + wetGain * H4(effect)
```

The low reference never passes through the effect or either level control.
Both levels muted therefore leave clean lows audible. L4 and H4 have the same
phase and sum to a unity-magnitude second-order allpass; there is no crossover
bump or dip for matching signals with gains summing to one. This is a gradual
24 dB/octave split, with the usual frequency-dependent crossover phase, not a
brick-wall or phase-free bypass. No sample buffer or transport latency is added.
The implementation reuses the double-precision trapezoidal SVF. It filters
ungained sources before applying levels, keeps filter history running while
disabled, and crossfades the toggle over 20 ms. The crossover's allpass phase
remains with Clean lows on even at unity Dry and muted Wet. With Clean lows
off, the original mixing returns: both muted are silent and unity Dry with
muted Wet is bit-exact, including signed zero.

Set Clean lows off for full-band phasing. Full-wet phasing preserves magnitude
while changing phase, so it can sound subtler than an equal dry/wet blend.

The normalized lattice topology bounds time-varying section energy. Feedback
magnitude never exceeds 0.75, excitation is bounded at ±16, and the feedback
sample at ±8. These emergency bounds are outside nominal audio levels.
The finite dry reference is preserved, including above ±16; protection-off unity Dry is exact.
Controls smooth over 20 ms; allpass coefficient targets update every 16 samples
and interpolate sample by sample. No audio-callback allocations or blocking.

There is no transport latency and the first output sample responds immediately;
the frequency-dependent allpass phase/group delay is the effect itself. Both
ports are mono. Separate instances are independent; stereo linking is not
implemented or advertised.

## Presets and contract

Slow Current provides restrained LFO movement; Expressive Envelope responds to
playing intensity; Wide Motion uses the full sweep. Every preset supplies all
input controls. URI: `https://suprduprnatural.github.io/supr-pedals/phase`.

```
0 in       1 out       2 rate       3 depth       4 centre
5 feedback 6 dry       7 protect    8 mode        9 sensitivity
10 wet
```

Effect Level is appended as port 10; all earlier port indices are preserved.
The face groups Motion, Voice and Envelope using the existing shared controls.
Tests cover analytical complex response, notch positions, sample-rate scaling,
LFO extrema, envelope calibration, automation, tail stability, real LV2
control mapping, in-place buffers, block partitions and lifecycle behaviour.
