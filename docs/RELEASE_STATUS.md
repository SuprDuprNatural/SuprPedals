# Integrated release — 7 September 2026

This release combines all eighteen plugins with the companion PiPedal custom
faces and host latency compensation. GitHub publication is held for Luke's
rig audition. See [RELIABILITY.md](RELIABILITY.md) for implementation contracts
and [ROADMAP.md](ROADMAP.md) for the deliberately deferred scope.

## Deployment

Target: `lukepi4@pi4.local` (Pi 4, 8 GB, 1.8 GHz). Browser:
`http://pi4.local`. The `.local` SSH key was verified against the trusted `pi4`
entry; use `-o HostKeyAlias=pi4 -o StrictHostKeyChecking=yes` on this machine.
Native sources are `~/SuprPedals` and `~/src/pipedal-codec-zero`.
Audio configuration is Codec Zero AUX, 44.1 kHz, 128 frames, four buffers,
analogue input gain −6 dB.

Installed successfully: `pipedald` is active and both LV2 discovery and the
live host report eighteen Supr effects, including Shape and Phase. Checksums
match every staged plugin/UI file and the native host binary. No error-level
journal entries followed installation. The deployed main asset is
`main-BMK7nAJH.js` (SHA-256
`fbe3d86352566d9f87e32116d362cbd7f40f83d70193852da31f05d002a4ca4c`);
host SHA-256 is
`6e6166d29d114e2f6b4788b1641064d5b5d8bd1bb1efa50cd156e9b49fd8f881`.

The fresh HPlants2 live backup restored exactly. The raw restore API does not
run PiPedal's saved-preset default migration: its old control list initially
caused a missing-control UI error. Appending only the five missing input
values fixed it: Clack `learn=0`, Compressor `detector=0`, and Fuzz
`match_mode=0`, `held_gain=0`, `learn=0`. All existing values were preserved;
the migrated board was verified equal after restoration. No presets were saved.
The deployed Learn/Held Peak/Held Match controls render on the actual Pi-backed
board, including a narrow viewport with no document overflow.

## Verification and release fixes

Local DSP, NAM graph/LV2/model tests, metadata checks, host latency regression
and UI build/component checks passed before native deployment. Pi host latency
regression and real-model NAM checks also passed. Native compilation exposed
Transient block-size variation (maximum sample difference 3.62e-5) under GCC
fast-math. Its wrapper and regression harness now share strict floating-point
flags; the unchanged exact-output regression passes under those flags.
The final `make test` passed on macOS and the Pi; native NAM graph, LV2 host
and real-model tests all report zero failures. Real-model A/B polarity nulls
were exactly zero inline and Threaded. Native RDF tooling is absent, so the
actual Pi-staged bundles were copied back and all eighteen were parsed and
validated with the existing local RDF environment.

At 44.1 kHz/128 frames, default sustained-sine offline probes processed ten
seconds in 0.0574 seconds for Echo (0.574% of one core) and 0.1107 seconds for
Space (1.107%). These are isolated default-setting measurements, not worst-case
board guarantees. Full-control/native regression suites cover Shape/Phase
stability and response.

The RDF port validator now asserts it inspected ports and uses `lv2['index']`
instead of the Namespace string method. UI staging copies the whole generated
site, and the companion CMake build now tracks Supr face sources.

Echo and Space omit duck meters. Echo has joined `1/4`, `1/8`, `1/8.` buttons
and Tap above them, with no Hold button. Space's Send is in its right column.
The hidden LV2 Hold/meter ports remain compatible with saved boards.

The restored live board was observed for three minutes (seven samples): zero
underruns throughout, CPU 16.1–17.2%. This was passive observation, not a
worst-case playing or multi-model stress test. Physical encoder/touch checks,
full-board stress and listening remain in the acceptance list below.

The companion PiPedal integration commit is `ec5e86ebf80494be958ca74b6f31a4e73351a0ac`.
The SuprPedals commit containing this report is its matching plugin release.
Both are local, unpushed commits. At the pre-release fetch, SuprPedals matched
`origin/main`; PiPedal had one existing local Codec Zero commit ahead of
`fork/main`. Preserve that commit when publishing the integration.

## Acceptance still owned by the player

Hard-refresh the browser after deployment. Audition the new effects, Learn
controls, Held Peak, Fuzz matching, bass transparency and Echo/Space Send tails.
For SuprNAM, load the same model into A and B, parallel routing, invert B:
check silence inline and Threaded while playing. Confirm split/bypass alignment
and sustained CPU/underruns on the intended real board. Offline null tests
prove sample order; they do not establish every real-time playing condition.
Only publish both repositories after this acceptance and any requested fixes.

## Recovery and continuity

The Pi's private `~/supr-release-20260907/backup/` contains the previous host,
LV2 bundles, UI, settings/pedalboard data and both source snapshots. Native
build/test logs sit beside it. Preserve these until audition is accepted.
To roll back, stop `pipedald`, restore the previous host and Supr bundles plus
`/etc/pipedal/react` from `system.tar.gz`, then restart. Restore preset data
only if needed: replacing it would discard changes made after the backup.
Do not delete unrelated LV2 plugins or replace the whole system tree blindly.

Completed handoffs were consolidated into durable design/reliability docs.
Their original text is retained privately in the local ignored
`build/release/handoffs-before-cleanup/`; models, audio and live-board backups
are deliberately excluded from Git.
