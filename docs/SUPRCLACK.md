# SuprClack

SuprClack reduces noises that are not part of the intended bass note: fret
clicks, finger scrapes, sympathetic undertones, shift squeaks and the noise
floor between notes.

The main rule is simple: when the detector is uncertain, it does nothing.
Missing some noise is preferable to damaging a note.

## Signal path

```text
input ──► 2 ms delay ──► harmonic sieve ──► HF duck ──► Delta/output
   │                           ▲                 ▲
   ├──► envelopes and bass gate ──┴─────────────┘
   └──► TunerDsp ──► period
```

The 2 ms lookahead is reported to the host. Neutral controls are bit-exact
against the delayed input.

Four mechanisms handle different types of noise.

## Clack: short high-frequency events

A fret click is both brighter than the current note justifies and brighter
than the recent signal. The detector calculates both excesses and uses the
smaller:

```text
transient excess = high band - recent high-band reference
ratio excess     = high band - (low band + allowance)
duck             = min(transient excess, ratio excess)
```

A transient-only detector softens every legitimate attack. A ratio-only
detector stays active on a naturally bright bass. Requiring both conditions
passes those cases and reduces only the disproportionate part of a click.

The duck acts above **Focus**. Two cascaded low-pass stages protect the band
below it. One pole left too much of a 3 kHz click in the protected path and
limited removal to about 9 dB; two poles allow more than 20 dB while leaving
the fundamental effectively unchanged.

## Scrape: the harmonic sieve

A scrape during a held note is not a transient, so an envelope cannot identify
it. It is, however, non-periodic at the note's period.

SuprClack reuses `TunerDsp` to obtain period `T` and builds a two-stage comb:

```text
H = (x + 2x[-T] + x[-2T]) / 4
R = x - H
```

For a periodic component, `x[-T] = x`, so `H` passes the note and all of its
harmonics with zero phase shift. Non-periodic material and an octave-under
component fall into residual `R`.

The residual is divided at `2.5 × f0`. Each band learns its own clean baseline
and responds only to increases above that baseline. A fixed threshold is not
safe: real strings are inharmonic, so clean upper partials leak into the
residual by different amounts on different notes.

The residual is reduced, not removed outright:

```text
output = delayed + (lowGain - 1) × lowResidual
                 + (highGain - 1) × highResidual
```

When both gains are one, the residual terms are exact zeros.

### Sieve guards

A comb tuned to the wrong period can remove the note. The sieve only acts when
all of these checks pass:

| Guard | Purpose |
| --- | --- |
| Confidence above 0.82 for 30 ms | Reject guessed pitches |
| No attack hold | Keep the sieve out of a new pluck |
| Pitch update newer than 60 ms | Do not treat the tuner's display hold as live evidence |
| Residual at least 4 dB below the harmonic estimate | Veto a plainly mistuned comb |
| Two periods settled at the current pitch | Let old delay history leave the taps |
| Residual gains exactly at unity before moving taps | Prevent discontinuities |
| 120 ms duck attack | Give the pitch guards time to respond first |

The period update continues even when the residual veto is active. Earlier
code stopped both; the stale period then kept the residual high and the sieve
could never recover.

Tap positions move only when the residual gains are exactly at unity. A
one-pole release never reaches zero by itself, so the gain state snaps to the
identity once it is negligible. Invalid pitch uses a fast 20 ms bail-out;
normal scrape release remains 120 ms.

## Squeak: noise between notes

When no pitch is locked, the comb has no useful period. After 120 ms without a
confident note, the Scrape control instead enables a slower high-frequency
disproportion duck. The delay prevents a brief loss of confidence during a
legato change from being mistaken for a gap. It disengages as soon as a note
locks again.

## Gate: the gaps

The gate is keyed by a fixed four-pole 1 kHz low-pass. Real bass notes,
including palm mutes and ghost notes, contain low-band energy; most clicks and
squeaks do not. Four poles reject an isolated 3 kHz click by roughly 40 dB in
the detector while retaining every normal bass fundamental. The audio path is
still full-range: this filter decides only whether a bass is being played.

This is an actual open/closed gate rather than a finite-ratio expander. It
opens at **Thresh**, closes 6 dB lower, and holds for 25 ms before closing so
waveform valleys and a tail hovering around the knob cannot chatter. The 0.75
ms opening ramp fits inside the existing 2 ms lookahead. Closing uses a
bounded, zero-slope fade: a natural decay takes the selected **Release** time
(65 ms by default), while a sudden hand mute closes up to 3.5 times faster.

**Range** is literal: 0 through 40 means exactly that many dB of attenuation.
A separate endpoint displayed as **∞** selects the true hard gate and reaches
exact digital zero once the smooth close completes. That is important before
heavy distortion: there is no tiny residual for a high-gain stage to turn
back into hiss. Range is continuous rather than a 41-detent trim, so its drag
travel and always-visible value readout match the other main controls.

## Delta

**Delta** outputs `delayed dry - processed`, which is exactly what the pedal
removed. Use it while setting the pedal. If the note is clearly audible in
Delta, reduce the relevant section or relax Sense.

The switch is crossfaded. `Delta + output` reconstructs the delayed dry signal
within `1.5e-8` in the suite. There is no output gain because every section
only removes signal.

## Measured behaviour

| Test | Result |
| --- | ---: |
| Clean tracked note through the sieve | 0.00 dB change |
| Inter-harmonic scrape | −9.3 dB |
| Octave-under undertone | −8.1 dB |
| Fundamental and third harmonic during those tests | 0.00 dB change |
| Clean pluck at Clack 100 | −0.61 dB |
| Fret click on the same pluck | −23.2 dB |
| Note beneath that click | 0.00 dB change |
| Ghost-note thump / equal-level 3 kHz click through gate alone | 0.0 / −159.2 dB |
| Range 40 after bass ends | Exactly −40.0 dB |
| Range ∞ after bass ends | Exact digital zero |
| Gap squeak / held bright partial | −9.5 / 0.00 dB |
| ±30-cent, 5 Hz vibrato | 0.65 dB maximum duck |
| Six note changes at full settings | 0.00037 excess curvature |

On a 32.6-second fingerstyle DI, the sieve's removal during normal playing was
52–82 dB below each frame. Most of its work occurred in tails and gaps, around
23 dB below those quieter frames. The clack section found 34 events at Clack
100, with a median duration of 1.7 ms and a net level change of −0.012 dB.

## Known limits

- A hard note change can duck the non-harmonic residual by about 2.6 dB for
  33 ms before the tracker catches up.
- The sieve is monophonic. On chords and double stops it normally stands down;
  the clack duck and gate still work.
- After a note change, the sieve needs roughly 150 ms to settle and arm. Fast
  runs receive less scrape reduction than held notes.
- The detector cannot know whether a slap is intentional. Slap players should
  lower Clack or reduce Sense.
- The four-pole gate key is bass-specific.
- Very low notes need two periods of history. The buffer is fixed and bounded,
  but it is the largest in the regular pedal set.
- A board containing SuprTuner and SuprClack runs their separate `TunerDsp`
  instances.

## Starting points

| Use | Settings |
| --- | --- |
| General cleanup | Defaults |
| Noisy hands, quiet room | Clack 70, Scrape 70, Thresh −50 dB |
| Fretless or slides | Scrape 100, Clack 30 |
| Clack only | Scrape 0 |

Set up with Delta enabled, play the material that causes trouble, and adjust
one section at a time. Clack and Scrape show peak-held reduction. Gate shows
instantaneous state: empty when open and full only when closed, without peak
hold or display animation.

## Future work

The lower-risk improvements are an output port that explains the sieve state,
a short clean-playing **Learn** pass for Sense, and pitch-region-specific
inharmonicity baselines. Sharing one pitch analysis across plugins would save
CPU but needs a host-supported scalar bus.

Polyphonic combs or an STFT residual could handle chords, but both increase the
cost and the damage caused by a bad detection. Test either offline against the
current corpus before adding it to the live path.

If a learned version is attempted, train the guard and gain decision rather
than a waveform-to-waveform denoiser. The existing features already describe
the problem. Sample-aligned raw/clean pairs can provide target band gains, and
their difference can be reused as a noise-event library for augmentation.

## Tests and tools

```sh
make test
./build/test_octaver --wavclack in.wav out.wav \
    [clack scrape sense focus thresh range release]
```

`--wavclack` reports duck events, sieve engagement, gate activity and
residual distributions. The decisive listening test is Delta, or the
delay-aligned dry signal minus the processed output. Audible pitched tone in
that difference is a regression even if aggregate levels look acceptable.
