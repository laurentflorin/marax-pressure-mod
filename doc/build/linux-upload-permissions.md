# Linux upload permissions for the ESP32-S3 board

The upload error `Could not open /dev/ttyACM0 ... Permission denied` usually means the USB CDC serial device is not readable/writable by your normal user account.

## Fix

1. From the repository root, run:

   ```sh
   sudo ./doc/udev/install-espressif-udev.sh
   ```

2. Unplug the ESP32-S3 board and plug it back in.
3. Retry the upload from Arduino IDE / PlatformIO.

If your distro still reports a permission error after the rule is installed, run this once as root:

```sh
sudo chmod 666 /dev/ttyACM0
```

## If the upload now says the port is busy

On Fedora/Bazzite, the board can sometimes be grabbed by `ModemManager`, a serial monitor, or `brltty`. Check and free it with:

```sh
sudo fuser -v /dev/ttyACM0
# or, if available:
sudo lsof /dev/ttyACM0
```

If you see a process such as `ModemManager`, `brltty`, or an IDE serial monitor, stop it and retry:

```sh
sudo systemctl stop ModemManager
sudo systemctl stop brltty
```

For a persistent fix on Fedora/Bazzite:

```sh
sudo systemctl disable --now ModemManager
sudo systemctl disable --now brltty
```

Then unplug and reconnect the ESP32-S3 board and retry the upload.

The udev rule in `doc/udev/99-espressif-tty.rules` grants normal users access to Espressif USB serial devices, which is the standard fix for ESP32 upload access on Linux.
