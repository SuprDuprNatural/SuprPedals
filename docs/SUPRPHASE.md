# SuprPhase

Mono bass phaser with LFO or envelope movement and optional clean-low
protection. Four normalized first-order lattice allpasses create two deliberate
moving notches. This is phase modulation, not a delay or stereo widener.

| Control | Behaviour |
| --- | --- |
| Rate | Free-running sine LFO, 0.03–5 Hz. No host transport synchronization. |
| Depth | 0–1; full depth sweeps ±2 octaves about Centre. Zero holds Centre. |
| Centre | 100–2500 Hz; swept centre clamps to 25 Hz–min(10 kHz, 0.22 × sample rate). |
| Feedback | −0.75 to +0.75. Injection scales by 1−abs(Feedback), deliberately controlling resonant gain. |
| Mix | Linear dry/wet interpolation, 0–1. Default 0.5 gives deepest cancellation with feedback zero and protection off. |
| Clean lows | Default On. Smoothly enables a complementary correction around 250 Hz. |
| Mode | LFO or Envelope, with a smoothed crossfade between their movement signals. |
| Sensitivity | −24 to +24 dB envelope detector gain; independent of audio gain. |

Envelope mode uses a rectified input follower with 8 ms attack and 180 ms
release. Its normalized movement is `2e/(e+0.1)−1`, where `e` includes
Sensitivity. Quiet input returns toward the low sweep endpoint; louder notes
open the sweep. Sensitivity gives soft playing useful travel. Rate continues
running internally in envelope mode, so changing modes does not restart it.
Activation resets the LFO to phase zero and clears all state.

## Mix, lows and stability

With protection off, output is `(1−Mix) × dry + Mix × wet`. Positive wet
polarity is intentional. At zero feedback, the allpass branch has unity
magnitude, while its phase creates two cancellations at a half blend. For
Centre 700 Hz at 48 kHz the notches are 290.12 and 1684.28 Hz; measured rejection
exceeds 90 dB. Feedback includes a one-sample loop delay, accounted for in
the complex-response tests. It changes both resonance and notch depth.

Protection uses `dry + Mix × H × (wet−dry)`, where H is two cascaded 250 Hz
first-order highpasses. Equivalently, the dry low reference is L=1−H. This
is an explicitly voiced complementary split, not a Linkwitz–Riley crossover:
it reconstructs identity when wet equals dry, and suppresses the phaser's
low-frequency correction at 12 dB/octave. It is not an exactly isolated brick
wall low band. Across tested static centre/feedback settings at Mix 0.5,
30.87 Hz measures +0.002 to +0.120 dB with protection on.

The normalized lattice topology bounds time-varying section energy. Feedback
magnitude never exceeds 0.75, excitation is bounded at ±16, and the feedback
sample at ±8. These emergency bounds are outside nominal audio levels.
The finite dry reference is preserved, including above ±16 at zero Mix.
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
5 feedback 6 mix       7 protect    8 mode        9 sensitivity
```

This is a new ten-port contract; no legacy pedal ports or presets changed.
The face groups Motion, Voice and Envelope using the existing shared controls.
Tests cover analytical complex response, notch positions, sample-rate scaling,
LFO extrema, envelope calibration, automation, tail stability, real LV2
control mapping, in-place buffers, block partitions and lifecycle behaviour.
