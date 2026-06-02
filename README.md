# LELIT MaraX Pressure Mod with brew by weight

This mod is a fork of [marax-pressure-mod](https://github.com/larszi/marax-pressure-mod). It is adapted for the MaraX V2 and includes brew by weight using the Felicita Arc.

## Implemented Extension
- Using [the guide of m1n1.de](https://www.m1n1.de/en/lelit-mara-x-v2-gicar-internals) the wirering from the Gicar to the Arduino was adapted (see wireing diagram)
- Using [the guide of m1n1.de](https://www.m1n1.de/en/lelit-mara-x-v2-gicar-internals) the display and Arduino are powered via the 12V output of the Gicar using a step down voltage converter.
- Using the Felicita Arc brew by weight was implemented. A simple lienear model, using current weight, current pressure and current flow rate is used to predict the final weight (when the pump is turned of there is still pressure in the group head, so the final weight is not when the pump is turned off)
- So the parameters for the linear model can be learned and constantly updated storage is needed, so a SD card is added to the build.
- SD card allows to store pre-programmed pressure profiles.
- Slightly larger display is used.
- Different pressure sensor is used, so no resistors are needed

### Similar projects 
- [Gaggiuino](https://github.com/Zer0-bit/gaggiuino)
- [MaraX Shot Timer](https://github.com/alexrus/marax_timer)

## License
Apache License Version 2.0
