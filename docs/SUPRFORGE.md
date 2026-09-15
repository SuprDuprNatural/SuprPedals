# SuprForge

Modern bass distortion with clean low-end weight, a two-stage saturated upper
band, and separate controls for bite and excessive top end. It is an original
voice informed by Darkglass reference captures, not a claimed circuit clone.

## Play it

Start with **Foundry**. Raise **Drive** for more saturation, raise **Tight**
if the sound is too woolly, then use **Weight** to balance the foundation.
**Bite** brings the upper-mid attack forward. Lower **Fizz** to remove hash
without undoing that midrange emphasis. Match **Level** to bypass.

| Control | Range | Default | Purpose |
|---|---|---|---|
| Drive | 0–10 | 6.5 | Amount of two-stage distortion; zero removes its contribution |
| Weight | −12 to +12 dB | 0 | Clean low-band level |
| Tight | 80–500 Hz | 180 Hz | Crossover; higher leaves more lows clean |
| Bite | −9 to +9 dB | +3 | 1.8 kHz post-drive bell |
| Fizz | 1.5–12 kHz | 5 kHz | Four-pole high cut; the endpoint is **Open** |
| Level | −24 to +12 dB | −3 | Final output trim |
| Comp | 0–10 | 3 | Low-band compression; zero is **Off** |
| Gate | −90 to −30 dB | −65 | High-band noise gate threshold; the floor is **Off** |

The gate intentionally leaves the clean low band alone. A gentle background
rumble is therefore not proof of a broken gate; use the existing cleanup pedal
for whole-signal silence. Lower Gate if quiet playing or decays disappear.
Use Comp sparingly at first; Weight is its manual makeup control.

| Factory preset | Character |
|---|---|
| Foundry | Defined, heavy default with an intact clean foundation |
| Blacksteel | Tighter, more aggressive attack with controlled top end |
| Molten | Lower crossover and maximum drive for a thick, ragged voice |
| Cold Iron | Less saturation and a more open top |

The custom PiPedal face groups Foundation, Forge and Output. Its meters read
low compression, gate attenuation and output peak directly from DSP ports.
Output turns red at 0 dBFS. The face uses segmented reduction/output meters,
a gate-state inlay beneath Gate, and the shared 1 dB detented Level control.
The controls retain normal keyboard, touch and hardware-encoder support
through the shared Supr components.

## Signal path

Input → 3 Hz DC blocker → 4× oversampling → LR4 low/high split.

- **Low:** held-peak compression → Weight.
- **High:** clean-input-keyed gate → pre-emphasis → asymmetric soft clipping
  → 6.5 kHz interstage filter → second soft clipping → inverse emphasis.
- High-pass the difference between the distorted and original high band at
  Tight, then add it to the original high band. This rejects newly generated
  intermodulation below the drive band.
- Sum the bands → downsample → Bite → Fizz → Level.

There is no full-band dry blend against the crossover's allpass phase. Both
bands undergo the same oversampling path and the post-sum EQ. With Drive and
Comp off, Weight/Bite/Level at zero, Gate off and Fizz open, the response is
magnitude-flat through the useful bass spectrum. It is not bit-exact bypass:
the DC blocker, crossover phase and oversampling filters remain.

The two clipping stages use first-order antiderivative antialiasing of
`x/sqrt(1+x*x)`. Its antiderivative is `sqrt(1+x*x)`; rationalizing the divided
difference avoids cancellation when adjacent samples are close. The first
stage stores its unbiased previous input so changing bias still maps silent
input to silent output. See the primary
[antiderivative-antialiasing reference](https://www.research.ed.ac.uk/files/34115216/bilbao_pdf.pdf).

Two cascaded 31-tap halfbands contribute 15 + 7.5 base-rate samples. One
alignment sample at the intermediate 2× rate makes the common round-trip
latency exactly **23 samples**: about 0.52 ms at 44.1 kHz or 0.48 ms at 48 kHz.
The distortion's intentional filtering and antialias averaging additionally
shape its phase; the latency port describes the common buffering delay.

The clean-input gate uses a 1 kHz key filter, 6 dB hysteresis, 35 ms hold,
0.5 ms opening and 65 ms closing. It is not a denoiser for noise under a note.
Low compression uses a 20 ms held peak, 25 ms attack and 180 ms release.

All audio storage is fixed. Control-filter updates follow sample count rather
than host block boundaries, and parameter moves are smoothed. DSP meters hold
output peaks for 100 ms and retain brief low-band gain reduction for the UI.

## Verification

`make test` includes `test_forge` and a dependency-free TTL/enum validator.
The Forge suite runs at 44.1, 48 and 96 kHz, checking reconstruction at three
crossovers, low B beside strongly driven mids, Weight/Bite/Fizz calibration,
compression distortion, gating, fast plucks, silence, block invariance,
extreme control changes and the actual LV2 wrapper including in-place audio.

At maximum Drive, a 6,107 Hz stress tone with default voicing measures worst
selected non-harmonic foldback below −57 dB at 44.1 kHz and −61 dB at 48 kHz.
The probe checks folded harmonic locations through order 80. This is a
defined stress measurement, not a claim that every possible signal is
alias-free. A 31 Hz tone alongside driven 997 Hz mids changes by less than
0.01 dB at the fundamental in the tested neutral-Weight/Comp configuration.

`build/test_forge --bench` measures ten seconds of audio at 48 kHz in 64-sample
blocks. Run on the Pi to judge its cost; a desktop figure does not establish
hardware deadline margin.

The reference set includes Alpha Omega Ultra neutral/Bite gain-7 NAM captures.
They provide comparisons of level-dependent harmonic behavior;
they do not establish that Forge matches the original device at every
setting. Models remain external and are not bundled.

## Integration

DSP: `src/ForgeDsp.h`; LV2 wrapper: `src/SuprForge.cpp`; metadata and factory
presets: `ttl/suprforge.ttl`, `ttl/forge-presets.ttl` and
`ttl/forge-manifest.ttl`. The URI is
`https://suprduprnatural.github.io/supr-pedals/forge` and the bundle is
`suprforge.lv2`.

The face lives in the companion fork's `SuprPedalViews.tsx` and
`SuprForgeDisplay.tsx`, registered by `ControlViewFactory.tsx`. Reconnects
resubscribe to all meter ports.

The corrected Pi 4 native build uses **8.79% of one core** in that benchmark
(0.879 s for 10 s of audio). Forge excludes `-ffast-math` and disables floating
point contraction: GCC's fast-math build failed exact block-partition and
wrapper/in-place equivalence tests. The strict build passes those tests at all
three sample rates. This flag choice is part of its tested build contract.
