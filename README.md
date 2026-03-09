# Pickle_Flow

Standalone PickleReef module for an NPN pulse-output flow meter on ReefNet.

## Features

- ESP32-S3 PlatformIO project with its own board definition
- LVGL + LovyanGFX status screen support for Waveshare ESP32-S3 Touch LCD 2.8
- CST328 touch input wired into LVGL pointer driver
- Wi-Fi + WebSocket module protocol (`reefnet.v1`)
- NPN pulse counting using interrupt on GPIO input
- Real-time flow telemetry (L/min, pulses/sec, total liters)
- Runtime tuning over WebSocket (`set_param` / `set_parameter`)
- Persistent calibration + settings via Preferences

## Default hardware mapping

- Flow pulse input: `GPIO15`
- Status LED: `GPIO2` (board builtin LED)
- LCD backlight enable: `GPIO5`

Display SPI/panel mapping follows the known-good `PickleWire` setup for this Waveshare board (ST7789 over `SPI2_HOST`).

`FLOW_SENSOR_PIN` is configured in `src/main.cpp` and defaults to `GPIO15` for the Waveshare ESP32-S3 Touch LCD 2.8 setup.

## NPN wiring notes

This firmware assumes an **NPN open-collector pulse output**:

- Sensor output transistor pulls line LOW on pulse
- ESP32 pin uses `INPUT_PULLUP`
- Interrupt edge: `FALLING`

Typical wiring:

- Sensor V+ -> sensor-rated supply (often 5V or 12V, per datasheet)
- Sensor GND -> ESP32 GND (common ground)
- Sensor pulse output -> `FLOW_SENSOR_PIN`

If the sensor output is not open collector, level-shift to 3.3V-safe logic before ESP32 input.

## WebSocket endpoint defaults

- SSID: `ReefNet`
- Password: `ReefController2026`
- Server: `ws://192.168.4.1:80/ws`
- Protocol: `reefnet.v1`
- Module ID: `PickleFlow.FlowMeter`

## Supported control parameters

Send in `payload.parameters` with `command: set_param` or `set_parameter`:

- `pulses_per_liter` (float > 0)
- `report_interval_ms` (200..10000)
- `flow_input_inverted` (0 or 1)
- `reset_total_liters` (1 to reset counters)

## Build

```bash
pio run
```

## Upload + monitor

```bash
pio run -t upload
pio device monitor
```
