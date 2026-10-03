# SuprCrush

Dry and Wet levels use dB relative to a 50/50 blend: **0 dB on both gives
unity for matching signals**. Both knobs centre at 0 dB, reach +24 dB and
mute at −∞. The mute endpoint is stored as −60 in LV2. They use the same
continuous knob and text readout as Vowel's Throat control.

Bass bitcrusher: continuous effective bit depth, fractional sample/hold clock,
compensated drive, envelope clock movement and optional clean-low protection.
Mono input/output, zero transport latency, no new dependencies.

## Play it

Start with **Pocket Sampler** for grain, **Arcade Bass** for coarser steps,
**Falling Pixels** for a clock that opens on attack then falls with the note,
or **Attack Corruption** for harder crushing on the attack. Try it after
OctavePlus or before a gently mixed Chorus. These are measured starting
points; final musical voicing needs listening on the player's rig.

| Control | Behaviour |
| --- | --- |
| Bits | 2–16 effective bits, continuous. Lower is coarser. |
| Rate | 200–48000 Hz sample/hold clock, capped at the host sample rate. |
| Drive | 0–24 dB into the quantizer, compensated afterwards. Brings quieter signals into the steps; high settings clip the wet input. |
| Sweep | −4 to +4 octaves of envelope clock movement. Positive opens on attack; negative closes on attack. Zero disables movement. |
| Sensitivity | −24 to +24 dB on the envelope detector only. |
| Release | 30–1000 ms envelope recovery; attack is fixed at 8 ms. |
| Tone | 400–18000 Hz post-crush low-pass cutoff. |
| Clean lows | Default on. Keeps a separate unity-gain clean low band below 250 Hz, independent of both levels. Compact button colour shows state. |
| Dry Level | Independent dry upper-band gain with Clean lows on, −∞ to +24 dB, default 0 dB. |
| Wet Level | Independent crushed return gain, −∞ to +24 dB, default 0 dB; controls only the upper band with Clean lows on. 0 dB gives half amplitude; −∞ mutes. |

## Signal contract

An input envelope controls the sample/hold clock over the signed Sweep range.
Its response is `e/(e+0.1)` after Sensitivity gain, so low-level playing still
has useful travel. Clock limits are 200 Hz and the host sample rate. The clock
keeps running through changes; linear interpolation locates each fractional
capture between input samples. Held samples are quantized with
`round(clamp(sample * driveGain, -1, 1) * 2^(bits-1)) / 2^(bits-1) / driveGain`.
This symmetric mid-tread law maps silence to exact zero without idle dither.
Aliasing and quantization distortion are intentional; there is no oversampling.

Tone filters the held signal.

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

Controls smooth over 20 ms. Expensive coefficients update every 16 samples,
independent of host blocks. State uses double precision and strict floating
point. No allocation, locks, I/O or threads occur in processing. Nonfinite
controls take defaults; nonfinite audio becomes zero. Wet excitation is
bounded at ±16 and pathological final float overflow is bounded. With Clean lows off, unity Dry and muted Wet preserve every finite input
float and signed zero exactly. With it on, the independent clean low band remains
audible even with both levels muted.

URI: `https://suprduprnatural.github.io/supr-pedals/crush`.
Ports 0–11: `in out bits rate drive env sensitivity release tone protect dry wet`.
The web face groups Crush, Envelope and Output using existing Supr controls;
standard hosts and the OLED use the same TTL contract.

## Verification

`make test-crush` checks the real wrapper and DSP, buffer partition and in-place
identity under automation, reset/reinitialization, silence, malformed controls
and audio, quantizer levels, fractional bits, compensated drive, predicted
alias frequency, tone attenuation, envelope direction changes, output trim,
clean low-B preservation, presets and TTL/enum/default agreement. Processing is
covered at 8, 44.1, 48, 96 and 192 kHz; detailed audio checks use 44.1/48/96 kHz.

Before the true dry/wet crossfade, local low-B RMS variation was at most
0.156 dB with protection on. Four preset renders of the existing recorded
bass DI peaked below −8.5 dBFS, without output clipping. These measurements
do not establish headroom for arbitrary inputs or entire pedalboards.

On the Pi 4 at 1.8 GHz, the 48 kHz/64-frame offline benchmark measured about
0.36% of one core. This was run alongside the native regression suite, not
measured as a whole-board CPU delta.

## Deployment verification

Installed on `lukepi4@pi4` with the complete matching UI. Both local and Pi
`make all test` gates passed with zero failures; the local address/undefined
behaviour sanitizer run was clean. The host reports 19 unique Supr URIs,
ten Crush input controls and four factory presets. All four bundle files and
126 staged UI files match their installed copies byte for byte. Existing host
and plugin binaries retain their pre-deployment hashes.

The original unsaved board was restored exactly after the service restart.
After the user saved a newer board during the pause, final verification
preserved that newer board and presets exactly. No new XRuns occurred during
final verification (the pre-existing count remained 12). The real face was
checked through the dev proxy and on the installed UI: keyboard adjustment,
factory-preset loading, aligned desktop rows and whole-column wrapping at
320 px. The final UI-only update did not restart audio.
