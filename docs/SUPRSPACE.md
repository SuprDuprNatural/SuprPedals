# SuprSpace

Dry and Wet levels use dB relative to a 50/50 blend: **0 dB on both gives
unity for matching signals**. Both knobs centre at 0 dB, reach +24 dB and
mute at −∞. The mute endpoint is stored as −60 in LV2. They use the same
continuous knob and text readout as Vowel's Throat control.

A mono filtered, ducked room/plate-style ambience for melodic bass. Place it
late in the chain, normally after drive and Echo. Independent Dry Level and
Wet Level add undelayed input and ambience. Mono output needs no stereo fold-down.

## Controls and presets

**Dry Level** and **Wet Level** range from −∞ to +24 dB.
Defaults are +4.2969 dB dry and −8.8739 dB wet. Output is `dryGain*input + wetGain*return`;
Dry +6.0206 dB, Wet −∞ is bit-exact passthrough, and both muted silence the output.
Wet Level never changes dry or the feedback network.
The Return column is Dry Level, Wet Level, Duck and a compact Send button with
its label inside and colour indicating state. **Decay** sets nominal broadband RT60 from 0.2–8 s.
**Tone** sets loop damping at 800–12000 Hz; darker settings shorten the measured
high-frequency decay. **Duck** attenuates the return from a clean-input detector,
with 2 ms attack and adjustable **Recovery** (50–1500 ms). Duck never alters the
feedback network. **Low cut** (40–600 Hz) filters the wet send with two high-pass
poles. **Predelay** is 0–150 ms; a base sample plus diffusion/network delays
remain when the knob is at zero. **Send** off fades excitation over 20 ms and
lets the tail decay. Host bypass cannot promise audible trails.

| Preset | Dry dB | Wet dB | Decay s | Tone Hz | Duck | Predelay ms | Low cut Hz | Recovery ms | Send |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Bass Room | 4.609 | -10.458 | .65 | 5500 | .5 | 8 | 180 | 250 | On |
| Dark Plate | 3.750 | -6.745 | 2.8 | 2800 | .7 | 28 | 220 | 450 | On |

These presets use one network: the plate setting is a darker, longer diffuse
ambience, not a physical plate simulation or a separate algorithm. Measured
recorded-bass renders do not clip.

## Network, timing and stability

Two Schroeder allpasses (4.7 and 12.7 ms, coefficient 0.5) diffuse the send into
an eight-line feedback delay network. Line lengths are rounded upward to prime
sample counts near 29.7, 37.1, 41.1, 43.7, 53.1, 61.7, 71.3 and 79.7 ms. Distinct
lengths and allpasses decorrelate reflections without modulation or stereo.
The Householder scattering matrix is `H = I − 2uuᵀ`, with each component of
`u = 1/√8`. Its orthogonality preserves energy. Each line applies a passive
one-pole low-pass and gain `10^(−3 × lineSeconds / Decay)`, strictly below one.
Alternating-sign injection and extraction vectors have unit norm.

Decay changes smooth the loop gains over 20 ms, without changing line lengths
or pitch. Predelay uses Echo's stationary-tap 20 ms crossfade. Send high-pass
filters reject DC; strictly lossy loops let residual DC die away. Extreme wet
states are bounded at ±16. Normal signal tests do not approach these limits.
The dry path is neither filtered nor delayed; wet reflection timing and phase
are intentional. Mix zero is bit-exact for finite input.

Buffers use 225,168 bytes at 48 kHz, 450,304 at 96 kHz. No callback allocation,
locking, file access or threads. Pi 4 offline cost is about 1.05% of one core at
48 kHz/64 frames, not a whole-board capacity guarantee. The plugin builds with
strict floating point and explicit subnormal flushing.

At 44.1/48/96 kHz, a nominal 1.4 s impulse tail loses approximately 66–67 dB
between equal energy windows separated by 1.4 s, including damping. Maximum
8 s decay survives sustained low-B input and rapid control transitions without
runaway tails or accumulating DC. See `test/test_space.cpp` for reproducible
measurements and the actual-wrapper, in-place and block-partition gates.

URI ends in `/space`; indices 0–11 are `in out dry decay tone duck predelay lowcut
recovery send duck_gr wet`. Port 10 is the ducking output control. The custom
face has no duck-reduction meter; Send sits below Duck in the right-hand
Return column, matching Echo. Dry/Wet replace the old Mix/Level symbols at their existing indices.
