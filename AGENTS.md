# Working on SuprPedals

Operational notes for an agent picking this up. The README is the user-facing
description; this is the stuff that is not obvious from the code and has cost
time to learn.

## Two repos, one product

| | |
|---|---|
| This repository | the LV2 plugins, DSP, TTL and test harness. |
| A PiPedal fork, commonly checked out beside it | the custom faces in `vite/src/pipedal/`. |

A change to a pedal's *face* is in the fork; a change to what it *does* is
here. Most jobs touch both.

Design docs: [DESIGN_NOTES.md](docs/DESIGN_NOTES.md) covers the DSP and its
measurements, [SUPRCLACK.md](docs/SUPRCLACK.md) covers the cleanup pedal in
detail, and [SUPRDESIGN.md](docs/SUPRDESIGN.md) is the shared guide for the
custom faces and implemented OLED path.

## Build and test

```sh
make            # all 17 Makefile bundles
make test       # the offline suite - must be 0 failures
make demo       # demo WAVs to build/demo/
make tools      # build/fuzz_probe, the measurement rig
```

Everything except SuprNAM is header-only with no dependencies, so it all
builds and tests on macOS. **`make test` is the gate** — it is fast, it covers
every pedal, and it catches the things that matter (bit-exactness, crossover
reconstruction, level laws).

## Deploying to the Pi

Run the applicable local build and tests before publishing. SuprPedals pushes to `origin`;
the sibling PiPedal fork pushes to **`fork`**, never its upstream `origin`.

Set the deployment target explicitly for each session, for example
`PI=user@pipedal-device.local`. Keep strict host-key checking enabled and
resolve any changed key out of band; never bypass host verification.

The Pi's `~/SuprPedals` is a plain source directory. Sync the complete current
sources (exclude `.git`, `build`, `models`), then build there. The matching
host build is `~/src/pipedal-codec-zero`; preserve its source/build before
syncing the companion fork. Use RAM-safe jobs and inspect current build flags.
Ordinary plugins use `make`; NAM needs its separate CMake build.

`vite/restage-supr-ui.sh` builds and stages the **entire** dist, including
chunks, CSS, fonts and static assets. It does not install or restart audio:

```sh
PI=user@pipedal-device.local \
RSYNC_RSH='ssh -o StrictHostKeyChecking=yes' \
bash /path/to/pipedal/vite/restage-supr-ui.sh
```

Before any install/restart, back up installed bundles, host binary/service,
configuration, complete UI and the **latest live pedalboard**. Saved presets do
not include unsaved board edits. The websocket API provides `hello`,
`currentPedalboard` and `updateCurrentPedalboard` (body `{clientId,pedalboard}`);
read `PiPedalSocket.cpp`/`PiPedalModel.tsx` before using it. Store private JSON
under ignored `build/`, restore it after restart and verify the original
controls/paths exactly, allowing only documented new defaults. Never overwrite
saved presets to obtain a backup or leave a temporary test board loaded.

Stage matching binaries/TTL/UI and use atomic replacement of running files.
Do not use an uninspected old `~/supr-deploy.sh`: historical versions shipped
only one JS file and installed stale plugins without rebuilding NAM/host.
After restart, hard-refresh the browser. Verify service active, **18** unique
Supr URIs, actual installed metadata and source-to-installed hashes. A count
alone cannot detect a stale build. Ignore shutdown-only `Bad file descriptor`
web-server messages; investigate continuing errors and sustained xrun deltas.

## Seeing the real UI without deploying

The fork's dev server proxies to a running Pi, which puts the real app, with
real plugins and real port values, on localhost:

```sh
cd /path/to/pipedal/vite
PIPEDAL_SERVER=http://pipedal-device.local npm run dev
```

This is the only way to properly verify a face. Measure the DOM with
`getBoundingClientRect` rather than eyeballing a scaled screenshot. When the
Pi is unreachable, replicate the drawing math in a standalone HTML harness.

## SuprNAM is different

It is the one plugin with real dependencies (NeuralAudio, NAM Core, RTNeural,
Eigen), so it uses CMake and builds separately:

```sh
make nam SUPR_CPU=cortex-a72   # cortex-a76 on a Pi 5
sudo make nam-install          # on the Pi
```

Measured on an 8 GB Pi 4 at 1.8 GHz: **~46% of one core per model**, so one
inline is comfortable and **two inline is impossible** — from two models up,
Threaded is not optional. First full build ~9 minutes. On a **4 GB** board add
`NAM_JOBS=2`; peak compiler RSS is 400 MB for `NAM/wavenet/model.cpp` and GCC
runs heavier than clang on that code, so four jobs risks the OOM killer.
`SUPR_CPU` matters — the inner loops are all NEON.

The worker threads do get real-time priority: `pipedald` runs unprivileged but
its unit file grants `RLIMIT_RTPRIO 95`. Check with:

```sh
ssh user@pipedal-device.local 'for t in /proc/$(pidof pipedald)/task/*; do \
  printf "%-16s %s\n" "$(cat $t/comm)" "$(chrt -p ${t##*/} | tr "\n" " ")"; \
done | grep snam_'
```

Expect `snam_bg` (and `snam_w0/w1`) at `SCHED_RR` 75.

**The one hardware test worth running after any SuprNAM change:** load the
same model into slots A and B, routing `A∥B`, flip `Ø` on B. You should hear
*silence* — not "quiet". Then turn Threaded on and play again; still silence.
That proves sample alignment, identical trims, the mix law and the DC blockers
end to end on the real build. Anything audible means the deployed build is not
what was tested.

All NAM plugins share one model folder — the server resolves
`pipedal_ui:directory` to `uploads/<directory>` without reference to the
plugin, so models are visible to TooB NAM and SuprNAM alike.

## Things that have bitten

**TTL port indices and the C++ port enum must agree**, contiguous from 0. They
are two hand-maintained lists of the same thing and nothing checks them at
build time. Verify after any port change:

```sh
python3 -c "
import re,rdflib
ttl='ttl/suprfuzz.ttl'; cpp='src/SuprFuzz.cpp'
rdflib.Graph().parse(ttl,format='turtle')
i=sorted(int(m) for m in re.findall(r'lv2:index\s+(\d+)',open(ttl).read()))
b=re.sub(r'//[^\n]*','',re.search(r'enum PortIndex[^{]*\{(.*?)\};',open(cpp).read(),re.S).group(1))
print(len(i), len(re.findall(r'PORT_\w+\s*=\s*\d+',b)), i==list(range(len(i))))"
```

**`test/validate_ttl.py` is SuprNAM-specific.** It asserts 42 ports, file
properties and model browsers, so it reports failures on every other pedal.
Its generic checks are fine; do not treat the rest as real.

**Removing a port renumbers everything after it.** Update the TTL, the enum,
the LV2 wrapper's `connect_port` and `run`, and the test harness together.

**The Makefile's test target uses `$(wildcard src/*.h)`** deliberately. It
used to list headers by hand and two were missing, so edits to them silently
did not rebuild the harness — a measurement came back byte-identical to the
run before it, which is the only reason it was noticed.

**Know which tuner contract you are changing.** The SuprTuner web face consumes
the plugin's `frequency`, `note`, `cents`, `confidence` and `strobe` output
ports; keep that analysis in the plugin. PiPedal's passive OLED tuner is a
separate implemented feature: it analyses a lock-free copy of the main input
only while the tuner screen is active, so it works without a tuner plugin in
the pedalboard. Do not add another detector to either path.

**Presentation belongs in the renderer.** The tuner's strobe runs at the base
phase rate in the DSP; the 2× visual speed is applied in the view, so another
renderer can choose differently. Keep that split.

When restoring a raw live-board backup across appended ports, add only missing
input controls from the deployed metadata defaults before calling
`updateCurrentPedalboard`; that API does not run saved-preset `UpdateDefaults`.
Verify every original value/path survives. Otherwise the face can throw
“Missing control value” even though the audio host accepts the board.
