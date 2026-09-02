# Build Info

## General MaraX Infos

### Overview of  the OPV area

![OPVarea](/assets/t_fitting.jpg)

### Marax GiCar MCU

A detailed overview of the GiCard unit of the MaraX V2 is availabe here: [the guide of m1n1.de](https://www.m1n1.de/en/lelit-mara-x-v2-gicar-internals)

## Build Information

```
Do not take this as a build guide, just a documentation of this mod!
```

## Wire Diagram

This is the overall circuit of the mod. I did not solder anything. I use a breakout board on the ESP32 to securely fit cables to it. I use some Dupont connectors in other parts as well as Wago connectors and crimp connectors.
![TWiring](/assets/MaraX_pressure_mod.svg)

## Pressure Sensor

![TFitting](/assets/t_fitting.jpg)
The T fitting is mounted between the pump and the pressure gauge T fitting.
On the other side of the T Fitting the pressure sensor is mounted.

> Note: Lelit does use 5mm fittings these are found on high pressure systems. Make sure you use the correct diameter.

## Dimmer

The dimmer is wired between the Gicar and the Pump. I use some hot glue to make sure the dupont headers do not disconnect from the dimmer module.

![Dimmer](/assets/dimmer.jpg)

### Idle boiler fill

Because the dimmer sits downstream of the GiCar's pump relay, the GiCar cannot
fill the boiler through a closed dimmer, and nothing tells it why the pump did
not run. `updateIdlePumpControl()` therefore only cuts the pump when the reading
is genuinely a full boiler: above 1 bar cold or 5 bar once steam is over 80 °C,
held for 400 ms, on a filter that has had a second to settle after being seeded.
Even then the cut is released after 45 s if the pressure no longer backs it up.

If the boiler ever fails to fill on start-up, the serial log says so directly —
look for a `[FILL]` line next to the `[DIMMER] 255 → 0` that cut the pump.

## Brew Switch and Relay

![Brewswitch](/assets/brew_switch.jpg)
I have connected the Nano to the original brew switch, this way the Nano can detect if a brew is active. I have used a relay to still "switch" the brew switch for the MCU of the Marax. This way the MCU of the Marax still thinks is stock and will also know if a brew is active and heat the boiler accordingly.

![Brewswitch_side](/assets/brew_switch_sied.jpg)
On the side of the brew switch you can see which wire/connector is COM and which is NO

![Relay](/assets/relay.jpg)
I just put the relay on top of the Gicar unit.

> Note, the MCU of the Marax will switch ground over this switch!

## Power

Using a step down converter, we can use the 12V output of the Gicar to power everything that needs 5V. For me it did not work to power the ESP32 like that via the 5V pin of the ESP32, normally it should work. I tried with a Arduino nano and it worked. The monitor, pressure sensor and SD card reader are all powered via the Gicar 12V output.

## ESP 32

The ESP32 as well as the step down converter are mounted in a box outside of the MaraX.
