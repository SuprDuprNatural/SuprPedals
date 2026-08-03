# Supr UI and hardware design

This is the shared guide for the Supr panels in the PiPedal fork and the
hardware display path. Keep the copies in these two repositories identical:

```text
SuprPedals/docs/SUPRDESIGN.md
pipedal/docs/SUPRDESIGN.md
```

The DSP, LV2 wrappers and TTL metadata live in SuprPedals. The custom web
panels and GPIO/OLED implementation live in the PiPedal fork.

## Web panels

### What the panels should do

- Show every useful control once.
- Prefer a compact, tall panel over a wide panel that wraps unpredictably.
- Group controls by the job they perform and by signal flow.
- Show live DSP state when it helps the user choose a control.
- Use LV2 metadata for ranges, tapers, units, steps and scale-point labels.
- Work in both themes, on touchscreens and from the keyboard.

Decoration should not compete with controls. A label, mark or meter stays only
when it adds information.

### Where the code lives

The main files in `pipedal/vite/src/pipedal/` are:

| File | Role |
| --- | --- |
| `ControlViewFactory.tsx` | Registers a custom view factory for each plugin URI |
| `SuprPedalViews.tsx` | Defines the regular pedal layouts |
| `SuprNamView.tsx` | Defines the larger NAM routing and model panel |
| `SuprPanel.tsx` | Builds bordered units, columns, sections and nested groups |
| `SuprControl.tsx` | Dispatches an LV2 port to the appropriate Supr control |
| `SuprKnob.tsx` | Continuous control |
| `SuprStepKnob.tsx` | Detented control |
| `SuprButton.tsx` | Toggle and two-position controls |
| `SuprSelect.tsx` | Categorical selector |
| `Supr*Display.tsx` | Meters, plots, tuner and other live displays |

`PluginControlView` still owns plugin discovery and values. `SuprPanel` maps
the control nodes it creates back to LV2 symbols, then rearranges them. This
keeps file browsers and unsupported control types working and limits the
custom behaviour to Supr plugins.

### Control selection

`SuprControl` uses `UiControl` metadata and a small number of explicit layout
overrides:

| Port | Control |
| --- | --- |
| Continuous dial | `SuprKnob` |
| Toggled port or two-point A/B port | `SuprButton` |
| Categorical scale points | `SuprSelect` |
| Small integer, declared `rangeSteps`, or explicit `step` | `SuprStepKnob` |
| Unsupported type | Original PiPedal control node |

Use `UiControl.valueToRange()` and `rangeToValue()` for interaction. They
preserve logarithmic tapers, integer rounding and declared steps. Use
`formatDisplayValue()` for readouts so scale-point names and units remain
correct.

During a drag, call `previewPedalboardValue`. Commit with
`setPedalboardControl` on release. Outside an active drag, render the value
from props so preset loads, automation and hardware controls remain
authoritative. Controls must use pointer capture, `touchAction: "none"`, ARIA
roles and keyboard input.

### Continuous knob marks

The standard knob has a 44 px body in an 80 px slot and a 270-degree sweep.
Marks are optional and semantic:

| Mode | Use |
| --- | --- |
| `none` | Normal continuous control |
| `endpoints` | Frame a range where min and max matter |
| `home` | Show a useful default, especially an off-centre or logarithmic one |
| `fill` | Show a growing amount such as Fuzz Sustain |

The visual position of a fill ring is rounded to the nearest dot; the port
value remains continuous. Do not require exact floating-point equality for a
visual mark.

### Stepped knobs, buttons and selectors

A stepped knob is for a value selected by detent rather than swept by feel:
octave offsets, output trims and calibration controls. The minimal variant
uses the lit detent as its indicator. The full variant adds a pointer and
readout when repeatable numeric values matter.

Buttons put state in the cap: raised and neutral when off, inset and accented
when on. Do not add a second LED that repeats the same state. Keep native
button semantics.

Use selectors for categories such as waveform, filter mode and routing. Do not
attach tooltips to dropdowns; their portal-based menu can leave a hover tooltip
stranded on screen.

### Panel layout

`SuprPanelUnit` is one bordered unit containing columns. Each column stacks
sections, and each section contains one or more rows. Nested row or column
groups handle layouts such as a meter beside a stack of knobs.

Rules worth preserving:

- Top-align sections. Centring columns of different heights makes their labels
  appear misaligned.
- Keep section label slots the same height across neighbouring sections. Hide
  redundant text with `visibility` when the space is still needed.
- Remove the label slot only when every relevant section is unlabelled.
- Keep a full-width display header no wider than the columns below it.
- Build the unit as a column containing the header and the wrapping column row.
  A `100%` child inside a shrink-to-fit flex container can expand the panel far
  beyond its content.
- Let whole columns wrap on a narrow screen. Avoid a single long row of
  controls.
- Select stock-control elements by DOM structure when necessary. PiPedal's
  Emotion class names are hashes and cannot be matched by source style name.

Measure layout with `getBoundingClientRect()`. A screenshot can hide width and
alignment errors after scaling.

### Colour

Use theme-specific colours:

| Meaning | Dark | Light |
| --- | --- | --- |
| Gain added, active value or detent | `#e88f4d` | `#d2691e` |
| Gain removed or cut | `#6fa7d8` | `#3773aa` |

Tracks, ticks and secondary text use low-opacity theme-relative greys. Fixed
black display inlays are fine where the display itself is intentionally a
piece of hardware, such as the tuner.

### Live displays

Plugin-specific displays subscribe to output ports with `monitorPort`. Remove
subscriptions on unmount and when the instance changes. Re-subscribe on every
arrival at model state `Ready`, even when the instance ID did not change. A
server restart or websocket reconnect destroys server-side monitor
subscriptions while ordinary controls continue to work, which otherwise
leaves a meter frozen with no obvious error.

Poll at the lowest rate that still reads cleanly. The current short-event
meters use 30 Hz, with peak hold in the DSP so they do not miss transients.

The web SuprTuner display consumes five of the tuner plugin's output ports:
`frequency`, `note`, `cents`, `confidence` and `strobe`. It unwraps strobe phase
across 0/1 before rendering motion. `level` remains available to other
renderers.

### Adding or changing a face

1. Put the control and output contract in the plugin TTL.
2. If the view needs live state, publish it from the DSP as an output port.
3. Add the layout in `SuprPedalViews.tsx`, or use a dedicated view for a large
   interface such as SuprNAM.
4. Use plain symbols for normal dispatch and `{ supr: "symbol", ... }` only
   for semantic overrides such as marks, detents or compact presentation.
5. Register the plugin URI in `ControlViewFactory.tsx`.
6. Run the PiPedal web build and test the face in the real app.

The development server can use a running Pi as its backend:

```sh
cd /path/to/pipedal/vite
PIPEDAL_SERVER=http://pipedal-device.local npm run dev
```

This provides real plugins, port values, presets and reconnect behaviour on
localhost. When the Pi is unavailable, use a small standalone harness for
drawing maths, then verify again in PiPedal.

## OLED and hardware controls

The OLED work described here is implemented in the PiPedal fork. The main user
guide is `pipedal/docs/GpioControls.md`.

### Supported rig

The standard setup uses one Raspberry Pi I²C bus:

| Device | Default address |
| --- | ---: |
| Four Adafruit seesaw parameter encoders | `0x36`–`0x39` |
| ANO navigation controller | `0x49` |
| 128×64 SSD1306 OLED | `0x3C` |
| Optional HT16K33 LED matrix | `0x70` |

Configure it from **Settings → Hardware → GPIO / I2C buttons, encoders and
display**. **Set up four encoders + navigation + displays** creates the normal
layout. Settings are stored under PiPedal's data root at
`config/GpioSettings.json`—normally `/var/pipedal/config/GpioSettings.json`.

The service account needs access to the `i2c` and `gpio` groups. Use 3.3 V on
the shared bus, enable `/dev/i2c-1`, keep every address unique, and prefer a
400 kHz bus for the complete encoder rig.

### OLED screens and navigation

The passive screen can be:

- four parameters;
- input or output waveform;
- built-in chromatic strobe tuner;
- blank.

The four-parameter dashboard follows the loaded effect chain and omits ports
marked hidden or output-only. It uses each port's declared steps, integer
range, scale points and logarithmic taper. A bottom bar shows the current
position in the chain.

The ANO wheel navigates four layers: Parameters, Effects, Presets and Settings.
Navigation and parameter activity temporarily replace the passive screen. When
the overlay timeout expires, the selected passive screen returns. This
overlay-first rule preserves immediate hardware feedback without losing the
chosen idle mode.

### Built-in tuner

The OLED tuner is independent of the SuprTuner LV2 plugin. It reads a lock-free
copy of the main input, so no tuner plugin is required in the pedalboard and
the audio path is never muted or altered. Analysis runs only while the tuner
screen is selected.

`src/GpioTuner.hpp` contains the bass-first 18–500 Hz NSDF analyser adapted
from SuprTuner. `Ssd1306Display::DrawTuner` in `src/Gpio.cpp` renders the frame:

- moving pitch-scaled strobe dots;
- a fixed centre reference and ±50-cent needle;
- a thicker centre needle within ±0.8 cent;
- ASCII sharp note names;
- frequency and signed semitone error.

The web tuner and OLED tuner share the same visual conventions, but they are
separate analysers with separate input contracts.

### Threading and I²C

The audio host writes waveform and tuner samples into lock-free rings. The
GPIO display worker reads those copies, runs tuner analysis, renders the
framebuffer and performs all OLED I²C writes. Audio callbacks and UI/model
callbacks must never draw or touch I²C.

Display entry points update mutex-protected snapshots. The worker chooses the
active overlay or passive screen on its next tick. Full OLED frames are sent
in small chunks that yield the shared bus between writes, keeping encoder
polling responsive.

The default OLED refresh interval is 200 ms and the supported minimum is
50 ms. Lower it for smoother tuner or waveform motion only after checking the
complete I²C rig. Parameter overlays are forced immediately; browser edits
wait for the next scheduled frame so they cannot flood the bus.

### OLED implementation map

| File | Role |
| --- | --- |
| `src/Gpio.hpp` | Settings, display modes and manager interface |
| `src/Gpio.cpp` | SSD1306 drawing, input workers, overlays and display worker |
| `src/GpioTuner.hpp` | Built-in pitch and strobe analyser |
| `src/PiPedalModel.cpp` | Hardware navigation, parameter dashboard and audio providers |
| `src/AudioHost.*` | Lock-free waveform and tuner sample capture |
| `vite/src/pipedal/GpioSettingsDialog.tsx` | Browser configuration UI |
| `docs/GpioControls.md` | Wiring, use and troubleshooting |

### OLED verification

Before calling a display change complete:

- run the PiPedal GPIO tests, including the bass-note tuner lock test;
- run `npm run build` in `pipedal/vite`;
- check Controls, Waveform, Tuner and Blank modes on the physical OLED;
- confirm parameter and navigation overlays appear and time out;
- verify encoder polling remains responsive during full-frame updates;
- check low B, flat, sharp and in-tune display behaviour;
- disconnect the OLED and confirm `pipedald` continues with a warning;
- run audio while the tuner refreshes and check for XRuns.

For an OLED that does not open, check power, common ground, SDA/SCL, bus,
address and permissions with `i2cdetect -y 1`. An SH1106 panel is not a drop-in
SSD1306: it needs controller-specific initialisation and a column offset.
