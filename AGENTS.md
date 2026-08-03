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
make            # all 12 Makefile bundles
make test       # the offline suite - must be 0 failures
make demo       # demo WAVs to build/demo/
make tools      # build/fuzz_probe, the measurement rig
```

Everything except SuprNAM is header-only with no dependencies, so it all
builds and tests on macOS. **`make test` is the gate** — it is fast, it covers
every pedal, and it catches the things that matter (bit-exactness, crossover
reconstruction, level laws).

## Deploying to the Pi

The examples below use `user@pipedal-device.local` as the SSH target. Replace
it and the source paths for the local environment. Key authentication and
passwordless `sudo` are expected.

```sh
# 1. source across (the Pi's ~/SuprPedals is a plain directory, not a clone)
cd /path/to/SuprPedals
rsync -az --delete --exclude '.git' --exclude 'build/' --exclude 'models/' \
      ./ user@pipedal-device.local:~/SuprPedals/

# 2. build natively on the Pi
ssh user@pipedal-device.local 'cd ~/SuprPedals && make -j4'

# 3. build + stage the UI from the dev machine
PI=user@pipedal-device.local bash /path/to/pipedal/vite/restage-supr-ui.sh

# 4. install both and restart
ssh user@pipedal-device.local '~/supr-deploy.sh'
```

Then **hard-refresh the browser**. `supr-deploy.sh` runs `sudo make install`,
copies the staged UI bundle into `/etc/pipedal/react`, and restarts
`pipedald`. It does *not* build SuprNAM (too heavy for every UI deploy).

Verify rather than trust:

```sh
ssh user@pipedal-device.local \
  'systemctl is-active pipedald; lv2ls | grep -c supr-pedals'
```

Expect `active` and `13`. Then check the deployed TTL actually contains the
change you made — an install can succeed while shipping a stale file. Ignore
`WebServer: ... Bad file descriptor` in the journal; that is the web server
shutting down during the restart.

**Restarting `pipedald` discards unsaved pedalboard changes** and reloads the
saved one. If you changed a plugin's ports, its saved control values may come
back at defaults — tell the user to check.

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
