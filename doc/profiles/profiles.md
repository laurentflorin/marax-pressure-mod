# Pressure Profiles

A profile is a **polyline**. Each point anchors a target pressure at an absolute
time from the start of the shot, and a per-point flag says how the curve gets
there:

| Mode | Meaning |
| --- | --- |
| `ramp` | Pressure moves linearly from the previous point to this one |
| `jump` | The previous pressure is held until this point's time, then steps to it |

Between them these cover every shape the machine can pull — a flat staircase, a
smooth declining curve, or a mixture: hold 3 bar through the pre-infusion, jump
straight to 9, hold, then ramp down to 6.

The firmware evaluates the curve on every loop iteration rather than once a
second, so a ramp is genuinely smooth rather than a finer staircase.

Profiles live on the SD card in `/profiles/*.csv`, up to
`MAX_PROFILE_POINTS` (16) points each, with pressure capped at 12 bar and time
at 600 s.

## The v2 format

Every line is `key,values…`, so unknown keys are ignored and the format can
grow later without breaking older firmware:

```csv
# marax profile v2
name,Mixed ramp and jump
target_weight,36.0
point,0.0,3.0,ramp
point,8.0,9.0,jump
point,16.0,9.0,ramp
point,26.0,6.0,ramp
point,34.0,6.0,ramp
```

- `name` — the display name, shown on the machine and in Home Assistant. It is
  independent of the file name.
- `target_weight` — **parsed and preserved, but never applied.** The
  brew-by-weight target comes from the display and is stored in Preferences;
  this field is kept only so hand-edited files do not lose it on a round trip.
- `point,<seconds>,<bar>[,ramp|jump]` — `seconds` is measured from the start of
  the shot, *not* a segment length. Points must be in ascending time order, and
  `ramp` is assumed when the mode is left off.

Two points may share the same time — that is how a vertical edge is drawn, and
it is equivalent to a `jump`.

A file needs at least two points. One point describes no span of time, so the
profile is rejected and the previously loaded one stays in force.

`sd_card_examples/profiles/mixed_ramp_jump.csv` is a worked example.

## The old four-step format

The original layout is still read, unchanged:

```csv
name,t1p,t1t,t2p,t2t,t3p,t3t,t4p,t4t,target_weight
default,4,8,6,12,9,15,7,5,36.0
```

Each pair is a pressure and the number of seconds to hold it. On load these
convert to five `jump` points, which reproduces the staircase exactly and keeps
the same total duration — an existing SD card keeps working with no changes.
Saving a profile from Home Assistant writes the v2 format.

## Editing

Three ways, in increasing order of comfort:

- **The machine's display.** The manual page has four fixed pressure/time
  fields. Those are hardware components in the Nextion firmware and cannot be
  extended from this repository — the compiled `.tft` ships here without its
  `.HMI` source — so manual editing stays four steps. They feed the same
  polyline through `applyManualStepsToProfile()`.
- **A text editor**, straight onto the SD card.
- **Home Assistant**, by dragging the curve. See
  [the Home Assistant guide](../homeassistant/homeassistant.md#editing-profiles).

Whatever the source, the brew page's curve preview and the pump follow the same
`profileTargetPressureAt()`, so the graph on the machine always shows what will
actually be poured.

## Testing changes

The profile maths needs no hardware. From the repository root:

```sh
sh test/run.sh
```

This lifts the parsing, interpolation and serialisation functions straight out
of `marax_esp32s3.ino` by brace matching, compiles them for the host and checks
them — including that the Lovelace card draws exactly the curve the firmware
runs, sample for sample, for every profile in `sd_card_examples/`.
