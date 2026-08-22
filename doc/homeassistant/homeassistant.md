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
| Profile | select | `marax/profile/name` | Options are the profiles on the SD card |
| Profile command result | sensor | `marax/profile/result` | Diagnostic; the answer to the last edit |

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

## Editing profiles

The controller also publishes the pressure profiles stored on its SD card and
accepts edits back over MQTT. The **Profile** select entity is enough to switch
profiles from a dashboard or an automation; the custom card below adds a curve
you can drag.

### Topics

| Topic | Direction | Payload |
| --- | --- | --- |
| `marax/profile/list` | published, retained | Comma-separated profile file names |
| `marax/profile/name` | published, retained | The loaded profile's file name |
| `marax/profile/active` | published, retained | The loaded profile as v2 CSV |
| `marax/profile/result` | published | `ok: …` or `error: …` for the last command |
| `marax/profile/select` | subscribed | A profile file name to load |
| `marax/profile/save/<name>` | subscribed | A v2 CSV body to write to `/profiles/<name>.csv` |
| `marax/profile/delete/<name>` | subscribed | Any payload; deletes that profile |

The same [v2 CSV](../profiles/profiles.md#the-v2-format) travels in both
directions, so what you edit is byte for byte what the pump follows.

A few rules the firmware enforces, each answered on the result topic:

- **Commands during a shot are refused**, not queued. An edit must not land
  seconds after the lever drops.
- **Profile names may only contain letters, digits, underscore and dash.** They
  become file names, and rejecting `.` rules out path traversal.
- **A body is validated with the same parser that will later read it back**
  off the card, so nothing can be accepted here and fail on reload.
- **The loaded profile cannot be deleted.** Select another one first.
- Saving the profile that is currently loaded reloads it immediately, so the
  change takes effect without touching the machine.

Switching profiles by hand, if you want it in an automation:

```yaml
action: mqtt.publish
data:
  topic: marax/profile/select
  payload: classic_espresso
```

### The curve editor card

`homeassistant/marax-profile-card.js` in this repository is a self-contained
Lovelace card — no HACS, no build step, no external dependencies.

1. Copy it to `<config>/www/marax-profile-card.js`.
2. **Settings → Dashboards → ⋮ → Resources → Add resource**, URL
   `/local/marax-profile-card.js`, type **JavaScript module**.
3. Add it to a dashboard:

   ```yaml
   type: custom:marax-profile-card
   # prefix: marax    # only if you changed the MQTT topic prefix
   ```

Drag a point to move it, tap an empty part of the graph to add one, and use the
row below the graph to type exact values, switch a point between ramp and jump,
or remove it. A filled handle is a jump, an outlined one a ramp. The first
point is pinned to t=0, since that is where the shot starts. **Save** writes
back to the profile you loaded; **Save as…** writes a new file.

The card reads MQTT through Home Assistant's websocket API, which is
**available to admin users only** — a non-admin will see a subscribe error
instead of the curve.

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
- **An edit reports `error: profile body rejected by the parser`** — most often
  points that are not in ascending time order, or fewer than two of them.
- **A renamed or removed entity lingers** — the discovery payloads are
  retained. Clear the old one with an empty retained message:
  ```sh
  mosquitto_pub -h 192.168.1.67 -u marax -P '<your-password>' -r -n \
    -t 'homeassistant/sensor/marax/<old-id>/config'
  ```
