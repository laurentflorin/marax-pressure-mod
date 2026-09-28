# Home Assistant Integration

The controller publishes its sensor values over MQTT and announces itself using
[Home Assistant MQTT discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery).
Home Assistant creates the entities on its own — there is **no YAML to add** on
the Home Assistant side.

## What shows up

Everything is grouped under a single device named **Mara X**:

| Entity | Type | Topic | Notes |
| --- | --- | --- | --- |
| Brew temperature | sensor (°C) | `marax/sensor/brewtemp` | |
| Steam temperature | sensor (°C) | `marax/sensor/steamtemp` | |
| Steam target temperature | sensor (°C) | `marax/sensor/steamtargettemp` | |
| Fast heat countdown | sensor (s) | `marax/sensor/fastheat_timer` | |
| Shot count | sensor | `marax/sensor/shots` | `total_increasing`, so it survives resets in statistics |
| Serial frame | sensor | `marax/sensor/debug` | Raw GiCar frame, filed as a diagnostic entity |
| Heating element | binary_sensor | `marax/sensor/heatingelement` | |
| Power | binary_sensor | `marax/sensor/power_state` | |
| Recipe | select | `marax/recipe/active` | Options are the recipes on the SD card, plus `(none)` |
| Brew by recipe | switch | `marax/recipe/mode` | The settings page's switch; off, the machine brews as a stock Mara X |
| Recipe command result | sensor | `marax/recipe/result` | Diagnostic; the answer to the last recipe command |

### Availability

Two levels of availability keep the history graphs honest:

- `marax/status` holds a retained `online` while the ESP32 is connected. It is
  the MQTT *last will*, so the broker flips it to `offline` on its own if the
  controller is unplugged or loses WiFi — the entities grey out instead of
  showing a stale reading forever.
- The temperature, fast-heat, heating-element and diagnostic entities are
  additionally gated on `marax/sensor/power_state` being `1`. When the machine
  itself is switched off the firmware publishes zeros, and without this gate
  every history graph would be dragged down to 0 °C overnight.

**Shot count** and **Power** are deliberately *not* power-gated: the shot count
is cumulative and stays meaningful while the machine is off, and the power
entity is the thing being reported.

## Setup

### 1. Broker credentials

If your Mosquitto container runs with `allow_anonymous true`, skip this step.
Otherwise add a user for the controller (this example reuses an existing
Mosquitto container that also serves zigbee2mqtt):

```sh
docker exec -it mosquitto mosquitto_passwd -b /mosquitto/config/passwd marax '<your-password>'
docker restart mosquitto
```

### 2. Firmware settings

In `src/marax_esp32s3/marax_esp32s3.ino`, fill in the WiFi and MQTT settings:

```c
#define mqtt_server   "192.168.1.67"   // the Mosquitto host
#define mqtt_user     "marax"
#define mqtt_password "<your-password>"
```

> These defines are checked in empty on purpose. Take care not to commit your
> real credentials.

Discovery is on by default. To publish the plain values without announcing any
entities, comment out `#define ENABLE_HA_DISCOVERY` in the same section.

Then flash the board (USB, or over the network if `ENABLE_OTA` is set).

### 3. Home Assistant MQTT integration

If Home Assistant already talks to this broker for zigbee2mqtt, there is
nothing to do — the Mara X device appears within a few seconds of the
controller connecting.

Otherwise: **Settings → Devices & Services → Add Integration → MQTT**, then
point it at the broker host (`192.168.1.67`), port `1883`, and the username and
password from step 1. Leave the discovery prefix at the default
`homeassistant`; sharing it with zigbee2mqtt is fine, because the device IDs
differ.

The device then lives at **Settings → Devices & Services → MQTT → Mara X**.

## Recipes

A recipe is one coffee, dialled in: grind setting, dose, yield, temperature
switch position and its own pressure curve. What each field does on the
machine, and the file format, are in [the recipe docs](../recipes/recipes.md).

- The **Recipe** select sets the machine up for a recipe — its curve for the
  pump, its yield as the brew-by-weight target — from a dashboard or an
  automation. Picking one also switches brew by recipe on. `(none)` selects
  no recipe.
- The **Brew by recipe** switch is the same as the one on the machine's
  settings page. Off, the machine brews as a stock Mara X — full pump, no
  weight cut-off — and the selected recipe is kept for when it goes back on.

The recipe card below is where recipes are made.

### Topics

| Topic | Direction | Payload |
| --- | --- | --- |
| `marax/recipe/list` | published, retained | Comma-separated recipe file names |
| `marax/recipe/active` | published, retained | The selected recipe, or `(none)` |
| `marax/recipe/home` | published, retained | The three home tiles, e.g. `ethiopia_guji,house_blend,` |
| `marax/recipe/mode` | published, retained | `ON` or `OFF` |
| `marax/recipe/body/<name>` | published, retained | Each recipe file as it is on the card |
| `marax/recipe/result` | published | `ok: …` or `error: …` for the last command |
| `marax/recipe/select` | subscribed | A recipe file name, or `(none)` |
| `marax/recipe/save/<name>` | subscribed | A recipe CSV to write to `/recipes/<name>.csv` |
| `marax/recipe/delete/<name>` | subscribed | Any payload; deletes that recipe |
| `marax/recipe/home/set` | subscribed | Up to three recipe names, comma separated, one per tile |
| `marax/recipe/mode/set` | subscribed | `ON` or `OFF` |

Every recipe is published in full, so the card has the whole library without
asking for it. `marax/recipe/list` is the authority on what exists: a body can
outlive its file on the broker if the file is removed from the card by hand,
and anything reading the bodies should check them against the list.

The recipe CSV travels unchanged in both directions, so what you edit is byte
for byte what is stored and what the pump follows. A few rules the firmware
enforces, each answered on the result topic:

- **Commands during a shot are refused**, not queued. An edit must not land
  seconds after the lever drops.
- **Recipe file names may only contain letters, digits, underscore and dash.**
  They become file names and topic levels, and rejecting `.` rules out path
  traversal.
- **A body is validated with the same parser that will later read it back**
  off the card, so nothing can be accepted here and fail on reload.
- **Saving the selected recipe reloads it immediately**, so the change takes
  effect without touching the machine.
- **Deleting the selected recipe** leaves the machine with none selected.

The controller holds **one command at a time** and drops a second that
arrives before the first is answered. Scripts that send several should wait
for each answer on `marax/recipe/result`; the card does.

Putting recipes on the tiles from an automation:

```yaml
action: mqtt.publish
data:
  topic: marax/recipe/home/set
  payload: ethiopia_guji,house_blend,decaf
```

### The recipe card

`homeassistant/marax-recipe-card.js` in this repository is a self-contained
Lovelace card — no HACS, no build step, no external dependencies.

**1. Find the config directory.** In a Docker install it is the host folder
mapped to `/config`:

```sh
docker inspect homeassistant --format '{{range .Mounts}}{{.Source}} -> {{.Destination}}{{"\n"}}{{end}}'
```

**2. Copy the card into `<config>/www/`.** That folder does not exist in a
fresh install — create it. Home Assistant serves it at `/local/`:

```sh
scp homeassistant/marax-recipe-card.js you@homeassistant:/your/config/www/
```

**3. Restart Home Assistant** if you just created `www/`. The `/local/` route is
registered at startup, so until then the file returns 404 — this is the usual
reason a freshly installed card does not appear. Files dropped into an existing
`www/` need no restart.

**4. Register it.** **Settings → Dashboards → ⋮ → Resources → + Add resource**,
URL `/local/marax-recipe-card.js`, type **JavaScript module**. If the menu has
no Resources entry, enable **Advanced Mode** in your user profile — it is
hidden otherwise.

**5. Hard-refresh the browser** (Ctrl+Shift+R). The card is cached aggressively.
When you update it later, bump a version in the resource URL
(`/local/marax-recipe-card.js?v=2`), or browsers keep serving the old copy.

Then add it to a dashboard:

```yaml
type: custom:marax-recipe-card
# prefix: marax    # only if you changed the MQTT topic prefix
```

To check where a failure is: open `http://<home-assistant>:8123/local/marax-recipe-card.js`
in a browser. A 404 means step 2 or 3. If it loads but the dashboard says
*"Custom element doesn't exist: marax-recipe-card"*, it is step 4 or 5.

At the top, the card has the **Brew by recipe** switch and the selected
recipe. Below, it lists every recipe with its dose, yield, ratio, grind and
temperature, and which home tile it is on.

- **Use** sets the machine up for a recipe and switches brew by recipe on.
- **Click a recipe** to edit it: name, grind, dose, yield (or type a ratio and
  the yield follows the dose), the temperature switch, the home-screen tile,
  and the curve.
- **The curve** is dragged: drag a point to move it, tap an empty part of the
  graph to add one, and use the row below it to type exact values, switch a
  point between ramp and jump, or remove it. A filled handle is a jump, an
  outlined one a ramp; the first point is pinned to the start of the shot.
- **Replace the curve with…** copies one of the built-in starting curves
  (classic 9 bar, pre-infusion ramp, blooming, declining, …) or another
  recipe's. The copy is independent: editing it never changes where it came
  from.
- **Home screen tile** puts the recipe on tile 1, 2 or 3. A tile holds one
  recipe, so the card says which recipe it would replace.
- **New recipe** starts from a flat 9 bar curve. The file name is made from the
  name when it is first saved, and stays the same if the recipe is renamed
  later.

Nothing reaches the machine until **Save**. A save and a tile change are sent
one after the other, each waiting for the machine's answer.

The card reads MQTT through Home Assistant's websocket API, which is
**available to admin users only** — a non-admin will see a subscribe error
instead of the recipes.

### Coming from profiles

Earlier versions kept pressure profiles in `/profiles` and had a profile
card, a *Profile* select and a *Profile command result* sensor. The firmware
no longer reads `/profiles`, and removes those two entities from Home
Assistant itself when it connects. Delete `marax-profile-card.js` from
`<config>/www/` and its resource entry. Old retained profile topics can be
cleared from the broker with:

```sh
for t in list name active result; do
  mosquitto_pub -h 192.168.1.67 -u marax -P '<your-password>' -r -n -t "marax/profile/$t"
done
```

## A ready-made dashboard

`homeassistant/dashboard.yaml` in this repository is a complete three-view
dashboard. Paste it into **Settings → Dashboards → *your dashboard* → ⋮ Edit →
⋮ Raw configuration editor**, replacing what is there.

| View | What it holds |
| --- | --- |
| Machine | Brew and steam gauges, power and heating tiles, shot count, the selected recipe and the brew-by-recipe switch, a fast-heat countdown that appears only while it is running, and a three-hour temperature graph |
| Recipes | The recipe card and the result of the last recipe command |
| Diagnostics | Every entity in plain rows, plus a reference table of the MQTT topics |

The Machine view leads with two conditional banners, because "no reading" has
two quite different causes here: the controller being offline (everything
unavailable, via the MQTT last will) and the machine simply being switched off
(temperatures unavailable by design, so they cannot drag the history graph down
to 0 °C). Only the Recipes view needs the custom card; everything else is
built-in cards.

The entity IDs assume Home Assistant slugged the device as `mara_x` — for
example `sensor.mara_x_brew_temperature`. Check yours under **Developer Tools →
States** with the filter `mara_x`. A repeated discovery can leave a `_2` suffix
behind; if that happened, delete the stale entity or search and replace the IDs
in the YAML.

## Troubleshooting

Watch what the controller actually sends:

```sh
mosquitto_sub -h 192.168.1.67 -u marax -P '<your-password>' -v -t 'marax/#' -t 'homeassistant/+/marax/#'
```

- **Nothing at all** — check the serial log for `[MQTT] setBufferSize failed` or
  WiFi errors. The controller retries the broker every 5 seconds.
- **Values arrive, but no device in Home Assistant** — the discovery payloads
  are missing. The serial log prints
  `[MQTT] Discovery publish failed for <entity>` when a payload does not fit in
  the send buffer; raise `MQTT_BUFFER_SIZE`.
- **Values stop during a brew** — expected. `updateMqtt()` skips publishing
  while a shot is running so the MQTT client cannot pace the pressure-control
  loop. Values resume within 5 seconds of the brew ending.
- **Entities stay unavailable while the machine is on** — the controller only
  sets `power_state` to `1` after it has seen a serial frame from the GiCar
  board, and clears it after 15 seconds of silence. Check the *Serial frame*
  diagnostic entity.
- **The card says "Cannot subscribe to MQTT"** — the logged-in Home Assistant
  user is not an admin, or the MQTT integration is not set up.
- **An edit reports `error: brew in progress`** — commands are refused during a
  shot by design. Try again once the lever is back.
- **An edit reports `error: recipe body rejected by the parser`** — most often
  points that are not in ascending time order, or fewer than two of them.
- **A recipe is on the card but missing from the list** — its file name has a
  character other than letters, digits, `_` and `-`, or it does not parse. The
  serial log says which: `[SD] scanRecipes: skipped …`.
- **The home tiles stay empty** — the display needs the tiles added to its
  HMI; see [the display changes](../ui/recipes_display.md). If they show text
  but never hide or show, check `HOME_PAGE_ID`.
- **A shot ignores the recipe** — brew by recipe is off (the header on the brew
  page says so), or no recipe is selected.
- **A command reports `error: no answer from the machine`** in the recipe
  card — the controller did not reply within eight seconds: it is offline, or
  was busy with another command and dropped this one.
- **A renamed or removed entity lingers** — the discovery payloads are
  retained. Clear the old one with an empty retained message:
  ```sh
  mosquitto_pub -h 192.168.1.67 -u marax -P '<your-password>' -r -n \
    -t 'homeassistant/sensor/marax/<old-id>/config'
  ```
