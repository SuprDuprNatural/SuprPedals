# SuprEcho

Dry and Wet levels use dB relative to a 50/50 blend: **0 dB on both gives
unity for matching signals**. Both knobs centre at 0 dB, reach +24 dB and
mute at −∞. The mute endpoint is stored as −60 in LV2. They use the same
continuous knob and text readout as Vowel's Throat control.

A mono filtered ducking delay for bass. Put it after drive and corrective EQ;
Space can follow it. Dry Level and Wet Level independently add undelayed
input and repeats: `out = dryGain*input + wetGain*return`. Both controls independently scale their path without changing sends or feedback. There is no stereo widening or mono fold-down penalty.

## Controls

- **Time:** 20–2000 ms, the quarter-note period. **Division** selects quarter,
  eighth (×0.5), or dotted eighth (×0.75). The shortest effective delay is 10 ms.
- **Feedback:** 0–0.92. Repeat gain is also reduced by the feedback filters;
  this is not an RT60 control.
- **Dry Level:** −∞ to +24 dB, default +4.0824 dB. Unfiltered, undelayed input gain.
- **Wet Level:** −∞ to +24 dB, default −7.9588 dB. Independent repeat gain, outside sends,
  feedback and ducking. Dry +6.0206 dB, Wet −∞ is bit-exact input passthrough.
  Both muted silence the output while the delay loop continues.
- **Duck:** clean-input detector attenuates the return, outside the feedback
  loop. Zero disables attenuation. At maximum, a sustained −20 dBFS input
  gives approximately 20 dB wet reduction. Attack is 2 ms.
- **Tone:** feedback low-pass, 800–12000 Hz. The first repeat has only the
  send filter; subsequent repeats darken progressively.
- **Low cut:** two cascaded one-pole high-pass filters on the wet send,
  40–600 Hz. Default 150 Hz rejects a 31 Hz input by about 28 dB.
- **Recovery:** detector release time constant, 50–1500 ms.
- **Send:** switch off to fade new wet input away over 10 ms, leaving repeats
  audible. The dry level still follows Dry Level. This is the internal trails contract: ordinary
  PiPedal host bypass fades the whole plugin output and cannot preserve tails.
- **Hold (LV2/MIDI control only):** set to 1 to stop new sends and extend
  existing repeats with 0.995 feedback. Filtering and DC removal remain active.
  Entry/exit are smoothed, and a ten-second timeout requires release to rearm.
  This is decaying repeat extension, not an infinite freeze. The custom face
  omits Hold; its existing 0/1 port remains available. Return it to zero to release.

**Tap** sits above the joined **1/4**, **1/8**, **1/8.** division buttons and
writes Time using two browser clicks, with a valid interval of
134–2000 ms. Keyboard activation also works. PiPedal's existing MIDI Tap Tempo
binding can target Time because its units are milliseconds. Division then
scales the tapped quarter-note period. No unsupported global transport/tempo
Atom port is advertised. Incoming MIDI clock is not implemented by this plugin.

Time changes use a 20 ms linear crossfade between two stationary interpolated
taps. A new request during a fade waits until that fade finishes; only the latest
request is retained. This avoids accidental pitch sweeps and fade restarts.
During changes two echo timings coexist briefly. There is no pitch-motion mode.

## Factory presets

Values are shipped starting points. Recorded-DI renders are measured, while
musical tuning should still be confirmed with a listening test on the target
rig.

| Preset | Time ms | Feedback | Dry dB | Wet dB | Duck | Tone Hz | Low cut Hz | Recovery ms | Division |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Thickener | 105 | .18 | 4.910 | -12.396 | .15 | 5000 | 160 | 200 | Quarter |
| Ducked dotted eighth | 500 | .45 | 3.167 | -5.036 | .75 | 3800 | 180 | 300 | Dotted eighth |
| Dark quarter throw | 650 | .72 | 2.923 | -4.437 | .35 | 1600 | 240 | 450 | Quarter |

All presets start with Send on and Hold off. For a dub throw, leave Send off,
briefly switch it on for the note, then off again. A hardware control that sends
both 1 on press and 0 on release can perform a momentary send.

## Implementation and evidence

`EchoDsp.h` owns the loop. `TimeSpaceDsp.h` contains the small utilities shared
with Space. All storage is allocated during LV2 instantiate; processing has no
allocation, locks, file access or threads. The two-second double buffer uses
768,032 bytes at 48 kHz (1,536,032 at 96 kHz). Dry latency is zero; echo delay is
intentional wet timing, not latency compensation. Sample rates 8–192 kHz are
bounded; 44.1/48/96 kHz are tested. Nonfinite controls use documented defaults,
finite extremes clamp to ranges, and nonfinite audio becomes silence. The wet
send clamps extreme input at ±16 and loop storage at ±8; ordinary audio remains
linear below those protection bounds. Subnormal states are flushed explicitly.

Pi 4 offline benchmark: about 0.54% of one core at 48 kHz/64 frames. This does
not establish whole-pedalboard headroom. Strict floating point is intentional:
`-ffast-math` would invalidate finite checks and deterministic processing.

URI ends in `/echo`; indices 0–13 are `in out time feedback dry duck tone lowcut
recovery division send hold duck_gr wet`. The former Mix/Level indices are
retained, with new symbols and percentage units. `duck_gr` remains a negative dB output
for hosts; the custom face has no duck-reduction meter. The Return column is
Dry Level, Wet Level, Duck, then a compact Send button. Its label stays inside
the button, with colour indicating state.
