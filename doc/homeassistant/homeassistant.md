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
- **A renamed or removed entity lingers** — the discovery payloads are
  retained. Clear the old one with an empty retained message:
  ```sh
  mosquitto_pub -h 192.168.1.67 -u marax -P '<your-password>' -r -n \
    -t 'homeassistant/sensor/marax/<old-id>/config'
  ```
