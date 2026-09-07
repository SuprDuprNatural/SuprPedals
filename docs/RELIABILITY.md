# Signal integrity and host integration

The plugin sources and their PiPedal faces are released together. See
[RELEASE_STATUS.md](RELEASE_STATUS.md) for deployment evidence and pending
acceptance, and [SUPRDESIGN.md](SUPRDESIGN.md) for renderer conventions.

## Saved controls and sonic changes

Compressor, Clack and Fuzz retain their original 10, 15 and 9 ports.
The audition-only appended detector, Learn, match and sieve-state ports have
been removed. Existing original controls keep their indices and symbols.
Compressor uses its original peak detector and large-meter face. Fuzz keeps
its original internal automatic normalization, with no new matching controls.

Clack's gate replaces the old finite expander. Range 0–40 retains its dB
meaning; the separate 41 endpoint selects digital mute. New-instance defaults
are Range 41 and Release 65 ms. Existing saved numeric settings remain valid.
Band Blend=0 now includes the matched crossover allpass phase, as does wet.
It is magnitude-flat, not an integer-delayed copy. This removes the former
intermediate-blend comb cancellation; host delay compensation cannot correct
arbitrary filter phase.

Octave direct, Envelope dry blend and Compressor's main signal avoid cascaded
unconditional DC blockers. Protection remains in detectors and nonlinear paths.
The neutral Octave → Envelope → Compressor dry chain is exact at tested low-B
frequencies. Sans's intentional voicing is unchanged.

## NAM worker contract

Threaded mode defers the complete wet graph, including trims, gates, filters,
models and mix. The fixed delay is the negotiated maximum block length,
independent of each call's frame count. Preallocated timestamped queues retain
sample ordering across irregular blocks. Expired output is discarded at its
original deadline; missing due samples and overflow increment overloads.
Oversized host calls produce counted missing deadlines rather than truncating
output silently. Inline processing subdivides large calls safely.

Mode requests fade out over 64 samples, wait asynchronously for worker
ownership, reset the timeline at silence, then prime and fade in. Only
lifecycle reset/stop may wait for workers. Parameter/model changes must stay
behind `beginBlock()` ownership checks. Parallel stages can run models on
separate cores; serial dependent stages remain sequential. Compute telemetry
is the last job's percentage of available block time, not whole-board CPU.
Overload indication is held for one second.

## PiPedal latency and bypass

`LatencyCompensation.hpp` supplies the production `CompensationDelay`,
`PathLatency` and `LatencyMerge` used by the prepared host graph. Reported
latencies accumulate in serial paths and nested splits; shorter merge branches
receive delay. Host soft bypass keeps delay history warm and uses delayed dry
samples. Internal plugin bypass remains plugin-owned. Fixed-block staging is
included in reported latency and initialized with zero history.

Storage is allocated at graph preparation, never in Process. Each delay line
is bounded to 65,536 samples/channel; dynamic taps crossfade for 64 samples.
Compensation limits propagate in the graph rather than pretending alignment
is exact beyond the bound. This compensates transport delay, not cabinet,
crossover or model phase. Keep manual phase/alignment controls.

Echo/Space retain audible tails by switching Send off while the plugin remains
enabled. Ordinary host bypass does not promise wet tails. Echo's UI Tap writes
the quarter-note millisecond period; Division scales it. There is no global
transport or incoming MIDI-clock implementation. The custom face omits Hold
and duck meters; existing LV2 Hold/output ports remain compatible.

## Regression checks

`make test` includes all ordinary effects and exact appended enum/TTL contracts.
RDF checks need `rdflib`; use `lv2['index']`, because Namespace.index resolves
to the Python string method and silently traverses no ports. Forge, Fuzz,
Clack, Transient, Echo, Space and Phase use strict floating-point builds where
finite checks or exact block equivalence require them.

`make nam-test` runs synthetic graph, actual LV2 host/state and optional
external-model checks. Its deterministic worker pumping proves sample order,
not real-time scheduling. Run `latencyCompensationTest` from the fork build
for impulse/nested/serial/dynamic/bypass-delay checks; also verify the actual
host paths on hardware. For NAM, the same model in A and B, parallel with B
inverted, must cancel inline and Threaded. Measure sustained board xruns and
worker priorities separately. Models and private audio/board backups must
never enter Git.
