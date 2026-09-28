# Recipes

A recipe is one coffee, dialled in: everything someone else needs to pull the
same shot you did.

| Field | What the machine does with it |
| --- | --- |
| Name | Shown on the home tile and at the top of the brew page |
| Grind | Shown on the tile and the brew page. The grinder is not connected, so this is guidance |
| Dose (g in) | Shown, and used for the ratio |
| Yield (g out) | **The brew-by-weight target** |
| Temperature (0 / I / II) | Shown. The Mara X switch is mechanical, so this is guidance |
| Pressure curve | **What the pump follows** |

Recipes are made and edited in Home Assistant with the recipe card (see
[the Home Assistant guide](../homeassistant/homeassistant.md#recipes)) and
stored on the SD card. Up to three of them can sit on the machine's home
screen.

## Brewing by recipe

**On the machine** — tap a recipe tile on the home screen. The machine loads
the recipe and opens the brew page. The panel beside the graph says what to
grind, how much to dose and where to put the temperature switch; pull the
lever and the shot follows the curve and stops at the yield.

**In Home Assistant** — press **Use** on the recipe card, or pick it with the
*Recipe* select entity. Same effect, minus the page change.

### Brew by recipe on and off

The settings page has one switch, **Brew by recipe** (also a switch entity in
Home Assistant):

| | Pump | Weight cut-off | Brew page |
| --- | --- | --- | --- |
| **On**, recipe selected | Follows the recipe's curve | At the recipe's yield | Recipe name, panel filled in |
| **On**, no recipe selected | Full power, as stock | None | "No recipe selected" |
| **Off** | Full power, as stock | None | "Brew by recipe off" |

With the switch off the machine brews exactly as it would without this mod.
The selected recipe is kept, so switching back on returns to it. Picking a
recipe — a tile, **Use**, the *Recipe* select — switches it back on, since
that is a request to brew by it.

The selected recipe and the switch both survive a power cycle.

### What changes the selected recipe

| Action | Effect |
| --- | --- |
| Tapping a tile, **Use**, or the *Recipe* select | That recipe is selected, and brew by recipe switched on |
| *Recipe* select set to `(none)`, or **Clear** on the card | No recipe selected |
| Saving the selected recipe in Home Assistant | Reloaded straight away |
| Deleting the selected recipe | No recipe selected |

Nothing changes during a shot: commands are refused while the lever is up, and
the switch snaps back if flipped mid-shot.

## Files

Recipes live in `/recipes/*.csv` on the SD card, at most 24 of them. The file
name is the recipe's identity — letters, digits, `_` and `-` only. A file with
any other name is skipped and logged.

Every line is `key,value`, so unknown keys are ignored and the format can grow
without breaking older firmware:

```csv
# marax recipe v1
name,Ethiopia Guji
grind,2.4
dose,18.0
yield,40.0
temp,II
point,0.0,3.0,ramp
point,8.0,3.0,ramp
point,8.0,9.0,jump
point,18.0,9.0,ramp
point,30.0,6.0,ramp
```

| Key | Value |
| --- | --- |
| `name` | Display name, up to 31 bytes. Defaults to the file name |
| `grind` | Free text up to 15 bytes: `2.4`, `14 clicks`, even `2,4` |
| `dose` | Grams in, 0–40 |
| `yield` | Grams out, 1–150. Left out, the shot is not cut by weight |
| `temp` | `0`, `I` or `II` (`1` and `2` work too) |
| `point` | `point,<seconds>,<bar>[,ramp|jump]` — see below. At least two |

`name` and `grind` run to the end of the line, commas included. Double quotes
and backslashes are replaced by `'` and `/`, because they would break the
display command that shows the text. Unknown keys are ignored by the firmware
and kept by the recipe card, so a note added by hand survives an edit.

A recipe that fails to parse — no curve, or points out of order — is left out
of the list entirely, so it can never end up on a home tile.

### The pressure curve

The curve is a **polyline**. Each point anchors a target pressure at an
absolute time from the start of the shot, and says how the curve gets there:

| Mode | Meaning |
| --- | --- |
| `ramp` | Pressure moves linearly from the previous point to this one |
| `jump` | The previous pressure is held until this point's time, then steps to it |

Between them these cover every shape the machine can pull — a flat staircase,
a smooth declining curve, or a mixture: hold 3 bar through the pre-infusion,
jump straight to 9, hold, then ramp down to 6.

- `seconds` is measured from the start of the shot, *not* a segment length.
  Points must be in ascending time order; `ramp` is assumed when the mode is
  left off.
- Two points may share the same time — that is how a vertical edge is drawn,
  and it is equivalent to a `jump`.
- At most 16 points, pressure capped at 12 bar, time at 600 s. One point
  describes no span of time, so a curve needs at least two.
- After the last point its pressure is held until the shot ends — by weight,
  or by the lever — so a curve shorter than the shot never drops the pump to
  zero.

The firmware evaluates the curve on every loop iteration rather than once a
second, so a ramp is genuinely smooth rather than a finer staircase. The brew
page's preview, the Home Assistant card and the pump all follow the same
curve, sample for sample.

### Home tiles

Which recipe sits on which tile is kept in `/recipes/home_slots.txt`, one line
with the three tiles left to right and an empty entry for an unused one:

```
ethiopia_guji,house_blend,
```

It lives apart from the recipes so a tile can only ever hold one recipe.
Deleting a recipe takes it off its tile. A tile naming a recipe that is not on
the card is simply not shown.

`sd_card_examples/recipes/` has three example recipes and a tile file.

## Why the curve is inside the recipe

A curve shared between recipes would mean that fine-tuning it for one coffee
quietly changes another. Keeping the curve in the recipe makes every recipe
self-contained, and dialling one in never touches the rest. New recipes start
from one of the recipe card's built-in curves or a copy of another recipe's.

## Testing

```sh
sh test/run.sh
```

The recipe parser, the curve maths, the home-tile text and the tile file are
tested on the host, and the recipe card is checked against the firmware: every
example recipe and built-in starting curve is read by both, field by field and
curve sample by curve sample, and must read back identically after the card
has saved it.
