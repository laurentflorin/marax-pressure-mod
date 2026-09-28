# A overview of the different UI pages

The photos show the pages before recipes. How the home, brew and settings pages
change for recipes — and which pages go — is in
[display changes for recipes](recipes_display.md).

## Home page

The home page displays the brew and steam temperature, further the bluetooth sign shows if the Felicita Arc scale is connected.

![HomePage_no_scale](/assets/home_scale_not_connected.jpg)
![HomePage_scale](/assets/home_scale_connected.jpg)

By touching the right top icon the settings page can be reached. By touching the coffe cup icon the brew page is opened.

Up to three recipe tiles fill the middle of the home page. Tapping one sets the machine up for that coffee — its pressure curve, and its yield as the target weight — and opens the brew page. The tile of the recipe in use is highlighted. Which recipes appear is chosen in Home Assistant; see [Recipes](../recipes/recipes.md).

## Settings Page

![Settings](/assets/settings.jpg)

On the settings page the other pages can be reached. Its one switch, **Brew by recipe**, decides whether shots follow the selected recipe. Off, the machine brews as a stock Mara X: full pump, no weight cut-off.

## Brew graph page

![BrewGraph](/assets/brew_page.jpg)

The brew graph page will automatically be shown if the brew leaver it switched to the "on/top" postion

When brewing by recipe the header shows the recipe's name, the graph previews its curve, and a panel beside the graph shows the grind setting, dose, target weight, ratio and temperature switch position, so whoever is at the machine knows what to do before pulling the lever. Otherwise the header says why no recipe applies.

## Cleaning Page

![CleaningPage](/assets/clean_page.jpg)

If the cleaning page is active, it will count the shot time of the cleaning flush as well as the current amount of flushes that have been done.

## Machine infos and further settings

![MoreInfo](/assets/more_info.jpg)
![MoreSettings](/assets/more_settings.jpg)

Machine infos shows the raw readings from the machine; further settings holds going home after a brew and the display brightness.
