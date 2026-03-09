# PickleFlow

ESP32-S3 flow meter firmware for Waveshare ESP32-S3 Touch LCD 2.8.

## What it does

- Reads pulse flow sensor input on GPIO15 (PCNT-based counting)
- Publishes status over WebSocket using `reefnet.v1`
- Displays three swipeable pages:
	- Main: LPH gauge + 1-hour average LPH
	- Chart: rolling LPH history (1h/6h/12h/1d/1w)
	- Stats: LPM, pulse rate, total volume + reset button

## Hardware defaults

- Board: Waveshare ESP32-S3 Touch LCD 2.x
- Flow pulse input: GPIO15
- LCD backlight: GPIO5
- Touch: CST328 (I2C)

## Calibration

- Flow calibration is fixed in firmware: `70.01 pulses/liter`.
- This value is reported in status setpoints and is not writable by control commands.

## Secrets setup

- Copy `include/secrets.example.h` to `include/secrets.h`
- Set your local Wi-Fi credentials in `include/secrets.h`
- `include/secrets.h` is intentionally git-ignored

## Control/API

- Full WebSocket command reference: [PickleFlow-Commands.md](PickleFlow-Commands.md)

## Build and flash

```bash
pio run
pio run -t upload
```

## Serial monitor

```bash
pio device monitor -b 115200
```
