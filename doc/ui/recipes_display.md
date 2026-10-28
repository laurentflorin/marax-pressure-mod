# Display changes for recipes

Recipes replace pressure profiles, so the HMI changes in four places: recipe
tiles on the home page, a recipe panel on the brew page, a single switch on the
settings page, and the two profile pages go. All of it is done in the Nextion
Editor and the `.tft` rebuilt — the compiled file in `display/` cannot be
edited from here.

A component the firmware writes to but the HMI does not have fails silently
(the firmware runs the display with `bkcmd=0`), so the firmware can be flashed
before or after the display. Two things are not forgiving, though: the page
indexes (see [Page indexes](#page-indexes)) and the waveform width, which the
firmware now takes to be 300 px.

## Pages to delete

- **`profile1`** — the profile list with the target weight. Recipes carry
  their own yield, set in Home Assistant.
- **`profile`** — the manual T1–T4 profile. Manual mode is gone; a recipe's
  curve is edited in Home Assistant.

Then remove what only they used, the Nextion Editor will point at any
leftover reference when compiling:

- In **Program.s**, the globals `pPEnabled`, `remoteEnabled` and
  `targetWeight`.
- The string variables `profileList` and `actProfile`, wherever they live.

Keep `profileModetxt`: the brew page still copies its header from it.

Add one global to **Program.s**:

```
int recipeMode=1
```

The firmware writes it at boot and on every display refresh, and the settings
page reads it when it opens.

## Home page

The home screen becomes the place a guest starts: pick a coffee, and the
machine is set up for it. The three recipe tiles take the middle of the screen,
where the logo is now.

```
480 × 320
┌──────────────────────────────────────────────────────┐
│  [cup]                 (BT)                [settings] │  y 8–72
│ ┌──────────────┐  ┌──────────────┐  ┌──────────────┐ │
│ │Ethiopia Guji │  │House blend   │  │Decaf         │ │
│ │18g > 40g     │  │18g > 36g     │  │17g > 34g     │ │  y 84–228
│ │Grind 2.4     │  │Grind 3.0     │  │Grind 2.8     │ │  r1 · r2 · r3
│ │Temp II       │  │Temp I        │  │Temp 0        │ │
│ └──────────────┘  └──────────────┘  └──────────────┘ │
│  [boiler] 89                             129 [steam] │  y 240–312
└──────────────────────────────────────────────────────┘
```

- **Shrink the top-row icons** (cup, Bluetooth, settings) to about 64 px so
  the row ends around y = 72. The cup keeps its job: open the brew page with
  whatever is set up.
- **Put the logo behind the tiles**, or make it the page background. Hidden
  tiles then reveal it, so a machine with no recipes on the home screen looks
  exactly as it does today.
- The tile of the recipe in use is drawn lighter with green text, so a glance
  says which coffee the machine is set up for. With brew by recipe off, no
  tile is highlighted. Tapping any tile switches it back on.

### Components

Add three **Button** components (a Text with a touch event works as well):

| Attribute                  | r1              | r2                | r3                |
| -------------------------- | --------------- | ----------------- | ----------------- |
| `objname`                | `r1`          | `r2`            | `r3`            |
| `x`, `y`, `w`, `h` | 8, 84, 148, 144 | 166, 84, 148, 144 | 324, 84, 148, 144 |

The same on all three:

| Attribute           | Value                 | Why                                                                                                                           |
| ------------------- | --------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| `vscope`          | **global**      | The firmware writes the tiles whichever page is open; a local component only keeps what was written while its page was loaded |
| `sta`             | solid colour          |                                                                                                                               |
| `bco` / `pco`   | 8518 / 65535          | Match`RECIPE_TILE_BCO` / `RECIPE_TILE_PCO`; the firmware sets them anyway                                                 |
| `bco2` / `pco2` | 17036 / 2016          | Pressed look                                                                                                                  |
| `font`            | a 20–24 px font      | Five lines have to fit in 144 px                                                                                              |
| `xcen` / `ycen` | 0 (left) / 1 (centre) |                                                                                                                               |
| `isbr`            | **1**           | The text uses`\r` line breaks, and long names wrap                                                                          |
| `txt`             | *(empty)*           | So nothing flashes up before the firmware fills it                                                                            |
| `txt_maxl`        | 120                   |                                                                                                                               |

**Touch Release Event** — untick *Send Component ID* and enter:

| Tile | Code                   |
| ---- | ---------------------- |
| r1   | `printh 23 02 52 01` |
| r2   | `printh 23 02 52 02` |
| r3   | `printh 23 02 52 03` |

That is a small custom packet (`#`, length 2, `R`, tile number) rather than a
plain touch event, so it does not depend on the component ids the editor hands
out. The firmware loads the recipe and sends the display to `page brew` itself
— do not add a `page` command here, or the brew page would open before the
recipe is loaded.

**Home page Postinitialize Event** — hides unused tiles the moment the page
opens, before the firmware's own update gets there:

```
if(r1.txt=="")
{
  vis r1,0
}
if(r2.txt=="")
{
  vis r2,0
}
if(r3.txt=="")
{
  vis r3,0
}
```

## Settings page

One switch replaces *Pressure Profiling* and *Remote Profile*, and the
*Pressure Profiles* button goes with the profile pages:

```
480 × 320
┌──────────────────────────────────────────────────────┐
│ ←                                                     │
│   Brew by recipe                          (  ●)      │  btRecipe
│                                                       │
│      ┌─────────────┐ ┌─────────────┐                  │
│      │ Machine     │ │ Further     │                  │
│      │ Infos       │ │ Settings    │                  │
│      └─────────────┘ └─────────────┘                  │
│      ┌─────────────┐                                  │
│      │ Cleaning    │                                  │
│      │ Mode        │                                  │
│      └─────────────┘                                  │
└──────────────────────────────────────────────────────┘
```

- **Delete** the *Pressure Profiling* and *Remote Profile* labels and their
  switches, and the *Pressure Profiles* button.
- **Rename** the first label to *Brew by recipe* and reuse its switch as
  `btRecipe` (a **Dual-state button**, the same pictures as before).

`btRecipe` **Touch Release Event** — untick *Send Component ID*:

```
recipeMode=btRecipe.val
printh 23 02 4D
prints btRecipe.val,1
```

That sends `#`, length 2, `M`, then the switch's value as one byte (0 or 1).

**Settings page Preinitialize Event** — so the switch shows the real state:

```
btRecipe.val=recipeMode
```

Off, the machine brews as a stock Mara X: full pump, no weight cut-off, and the
brew page says *Brew by recipe off*. The selected recipe is kept for when it
goes back on. Flipping the switch while a shot is running is ignored — the
firmware sets it straight back.

## Brew page

The brew page gets a panel beside the graph with what to do before pulling the
lever, and the header names the recipe.

```
480 × 320
┌──────────────────────────────────────────────────────┐
│ ←  Ethiopia Guji                                      │  profileMode, full width
│ (drop)[Done]      00:00                               │
│ ┌──────────────────────────────────┐  Grind     2.4  │
│ │                                  │  In       18 g  │
│ │  s0  300 × 180                   │  Out      40 g  │  x 344–472
│ │                                  │  Ratio   1:2.2  │
│ │                                  │  Temp       II  │
│ └──────────────────────────────────┘                  │
│  Bar 00.00    Temp 86                                 │
└──────────────────────────────────────────────────────┘
```

- **Delete the static "Profile:" label** and widen `profileMode` to the full
  header (x 56, y 4, w 416, h 36). The firmware writes the recipe's name
  there, or *No recipe selected* / *Brew by recipe off*. With no scale
  connected it adds the time that will end the shot, as in *Ethiopia Guji
  (27 s)*, so give `profileMode` and the global `profileModetxt` a `txt_maxl`
  of at least 40: a 31-character name plus the time.
- **Narrow the waveform `s0` from 350 to 300 px** and move it to x 32 (its axis
  labels to the left of it), which frees x 344–472 for the panel. The firmware
  now draws the curve preview 300 samples wide (`BREW_WAVEFORM_WIDTH`); with
  the graph left at 350 px the preview stops short of its right edge.
- Label the recipe temperature **"Temp"** or **"Switch"**, but keep it visibly
  apart from the brew-water temperature in the footer (86 °C), which is a
  reading, not a setting.

### Components

Five **Text** components for the values, plus five static Text labels
(`Grind`, `In`, `Out`, `Ratio`, `Temp`) the firmware never touches:

| `objname` | Example   | Shows                                  |
| ----------- | --------- | -------------------------------------- |
| `rGrind`  | `2.4`   | The recipe's grind setting             |
| `rDose`   | `18 g`  | Grams in                               |
| `rYield`  | `40 g`  | Grams out — the brew-by-weight target |
| `rRatio`  | `1:2.2` | Yield ÷ dose                          |
| `rTemp`   | `II`    | Temperature switch position            |

Positions, one row per value, 36 px apart from the top of the graph:

|                               | x   | w  | rows (y, h 36)               |
| ----------------------------- | --- | -- | ---------------------------- |
| Labels                        | 344 | 52 | 95, 131, 167, 203, 239       |
| Values`rGrind` … `rTemp` | 400 | 72 | the same rows, in that order |

For each value: `txt` empty, `txt_maxl` 16, right-aligned (`xcen` 2), local
scope is fine. With no recipe in charge the firmware writes `-` into all five.

The values are rewritten every time the brew page opens — from a tile, the cup
icon, or the lever — and whenever the recipe or the switch changes while it is
open.

## Page indexes

The firmware tells pages apart by the index `dp` reports, and deleting a page
renumbers every page after it. After deleting `profile1` and `profile`, read
the new indexes off the editor's **Page** panel (the first page is 0) and set
them near the top of `marax_esp32s3.ino`:

```c
#define HOME_PAGE_ID 1       // shows and hides the home tiles
#define BREW_PAGE_ID 2       // curve preview, recipe panel
#define CLEANING_PAGE_ID 4   // cleaning mode: flush counter, no boiler cut-off
```

A wrong `CLEANING_PAGE_ID` matters most: pulling the lever on the cleaning
page would open the brew page and run the selected recipe's curve instead of
counting a backflush at full pump. The serial monitor prints the current page
as `page:` every ten seconds, so each one can be checked by opening it.

Do not go by the `printh 23 02 50 XX` in each page's Preinitialize Event: it
was typed in by hand and still carries the numbers from before the profile
pages went (the cleaning page prints `06`). The firmware ignores it.

## Checking it

1. Flash the display and the firmware; set the page indexes first.
2. In Home Assistant, put two recipes on tiles 1 and 2. They appear on the
   home screen within a second or two; tile 3 stays hidden.
3. Tap one. The brew page opens with the recipe name at the top, its curve in
   the graph and the panel filled in; the serial log shows `[RECIPE] Loaded '…'`.
4. Back on the home screen, that tile is highlighted.
5. On the settings page, switch *Brew by recipe* off. The log shows
   `[RECIPE] Brew by recipe off`, the tile loses its highlight, the brew page
   says *Brew by recipe off*, and the switch in Home Assistant follows.
6. Open the cleaning page and check the serial log reports its index.
