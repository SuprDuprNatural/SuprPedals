# SuprShape

Clean mono bass EQ with optional input-keyed dynamic cuts. Defaults are
sample-for-sample neutral, including DC: there is no hidden high-pass,
saturation, normalization, limiter or delay. Turn down Level when boosting
to leave headroom for the following plugin.

## Controls

| Controls | Contract |
| --- | --- |
| HPF | 0 = Off; positive values use a 12 dB/octave Butterworth high-pass. Effective corner is at least 10 Hz, maximum 100 Hz. |
| LPF | 20000 = Off; lower values use a 12 dB/octave Butterworth low-pass, 1–20 kHz. Corners clamp below Nyquist at low sample rates. |
| Bass / Treble | ±12 dB shelves, fixed half-gain frequencies 90 Hz / 3.5 kHz, Butterworth shape. |
| Mid Hz / gain / Q | 150–4000 Hz bell, ±12 dB, Q 0.3–4. Q specifies prewarped half-gain bandwidth. |
| Level | −18 to +6 dB, independent of the detectors. |
| Boom Hz / cut | 50–350 Hz, maximum downward reduction 0–12 dB; zero disables the cut. |
| Harsh Hz / cut | 800–6000 Hz, maximum downward reduction 0–12 dB; zero disables the cut. |
| Threshold / Release | Shared by both dynamic bands: −60 to 0 dBFS detector threshold and 50–1000 ms release time constant. |

The dynamic bands use Q=3 bandpass detectors on the original input, followed
by rectified peak envelopes (5 ms attack). Reduction rises from zero at
Threshold to the selected maximum at 12 dB above it. Threshold refers to the
smoothed detector amplitude, not an RMS or instantaneous full-band reading.
The two Q=3 audio bells apply only negative gain, without makeup gain. Static
EQ, Level and cut depth do not change detector calibration. The meters show
the actual smoothed bell-centre gain in negative dB, not a broadband level
measurement. This is selective boom/upper-mid processing, not general denoising.

## Processing and measurements

Signal order: optional HPF → optional LPF → bass shelf → mid bell → treble
shelf → dynamic boom bell → dynamic harsh bell → Level. The implementation
reuses the existing topology-preserving `Svf` in `SansDsp.h` without modifying
it. Frequencies, gains and bypass blends smooth with a 20 ms time constant;
dynamic gain targets update every 16 samples, independently of host blocks.
No allocation, locks, file access or other host dependencies occur in processing.

At 44.1, 48 and 96 kHz, tests measure a +6 dB bass shelf at +6.000 dB in the
low-frequency passband and +3.000 dB at 90 Hz. A 40 Hz HPF measures −3.010 dB
at its corner and −24.099 dB at 10 Hz; corner phase is +90°. The default impulse
is bit-exact and undelayed. Active EQ intentionally changes phase and group
delay; zero transport latency does not mean phase-neutral EQ.

The Low-B Cleanup preset uses only a 15 Hz HPF: loss at 30.87 Hz is 0.224 dB.
Room Boom uses a selective 120 Hz cut; Articulate Fingerstyle uses restrained
static boosts and a −3 dB trim; Controlled Pick uses a selective 2.5 kHz cut.
All presets specify every input control. Default dynamic depths are zero.

Rumble-control contract: leave HPF off unless deliberate rumble
removal is wanted. Do not add another unconditional dry DC blocker here or
compensate for other pedals' low-frequency losses in this EQ's neutral state.

## LV2 and verification

URI: `https://suprduprnatural.github.io/supr-pedals/shape`. New mono plugin;
18 contiguous ports, no changes to existing plugin contracts:

```
0 in          1 out        2 hpf        3 lpf
4 bass        5 treble     6 mid_freq   7 mid_gain
8 mid_q       9 level     10 boom      11 boom_freq
12 harsh     13 harsh_freq 14 threshold 15 release
16 boom_gr   17 harsh_gr
```

Ports 16–17 are hidden output controls, −12 to 0 dB. The custom face uses
shared knobs, output detents and two reconnect-aware reduction meters.
`make test` includes calibration, cutoff/slope/phase, low-B, detector isolation,
quiet release, rapid automation, invalid input, reset, partition and actual
in-place LV2 tests. See [release status](RELEASE_STATUS.md) for current native
CPU, UI and rig acceptance evidence.
