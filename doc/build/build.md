# Build Info


## General MaraX Infos
### Overview of  the OPV area


![OPVarea](/assets/info_opv_area.jpg)


### Marax GiCard MCU

A detailed overview of the GiCard unit of the MaraX V2 is availabe here: [the guide of m1n1.de](https://www.m1n1.de/en/lelit-mara-x-v2-gicar-internals)

## Build Information
```
Do not take this as a build guide, just a documentation of this mod!
```

This is the overall circuit of the mod. I did not solder anything. I use a breakout board on the Nano to securely fit cables to it. I use some Dupont connectors in other parts as well as Wago connectors and crimp connectors. 
![TWiring](/assets/wiring_diagram.png)

## Pressure Sensor

![TFitting](/assets/t_fitting_mount.jpg)
The T fitting is mounted between the pump and the pressure gauge T fitting.
On the other side of the T Fitting the pressure sensor is mounted. 

> Note: Lelit does use 5mm fittings these are found on high pressure systems and can to ~18bars
> They can be hard to find but Festo also does use 5mm hosing.
> 
> NOT 4mm NOT 6mm but 5mm! I ordered a lot of wrong parts because of this.

I have also added some extra foam for the pressure sensor to keep it from making noise.

The pressure senor works with max 4,5 Volts for 12 Bar. This is to much for the Nano which works with 3,3Volts for this I have added a voltage divider with two resistors.
>0.3V for 0 Bars
>3V for 12 Bars


![Pumpwires](/assets/pump_wires.jpg)

The AC dimmer will be connected between the pump and the cables from the MCU of the MaraX.
>Remember the pump runs on mains 230V power!!
The AC dimmer will always allow 100% of the power to the pump if no brew is active, this way the MCU of the Marax can still control the pump to fill the boiler etc.

![Brewswitch](/assets/brew_switch_wires.jpg)
I have connected the Nano to the original brew switch, this way the Nano can detect if a brew is active. I have used a relay to still "switch" the brew switch for the MCU of the Marax. This way the MCU of the Marax still thinks is stock and will also know if a brew is active and heat the boiler accordingly 
> Note, the MCU of the Marax will switch ground over this switch!
