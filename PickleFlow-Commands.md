# Pickle Flow WebSocket Commands

## Shared envelope

```json
{
  "protocol": "reefnet.v1",
  "module_id": "PickleFlow.FlowMeter",
  "submodule_id": "PickleFlow.FlowMeter",
  "type": "status|control",
  "sent_at": "2026-03-09T12:34:56Z",
  "payload": {}
}
```

## Outbound status payload

`type: "status"`

```json
{
  "uptime_s": 123,
  "firmware": { "version": "0.1.0", "build": "..." },
  "network": { "ip": "...", "mac": "...", "ssid": "ReefNet", "rssi_dbm": -58 },
  "environment": { "temperature_c": null },
  "subsystems": [
    {
      "key": "flow",
      "label": "Pickle Flow",
      "kind": "sensor",
      "submodule_id": "PickleFlow.FlowMeter",
      "state": "idle|flowing",
      "badge": "ok|active",
      "setpoints": {
        "pulses_per_liter": 70.01,
        "report_interval_ms": 1000,
        "flow_input_inverted": false
      },
      "sensors": [
        { "label": "Flow Rate", "value": 3.25, "unit": "L/min" },
        { "label": "Pulse Rate", "value": 24.4, "unit": "Hz" },
        { "label": "Total Volume", "value": 12.8, "unit": "L" },
        { "label": "Total Pulses", "value": 5760, "unit": "count" },
        { "label": "Ignored Bounce", "value": 2, "unit": "count" }
      ]
    }
  ]
}
```

## Inbound control

`type: "control"`, `payload.command` values:

- `set_param`
- `set_parameter`
- `config_request`
- `module_manifest_request`
- `status_request`
- `ping`

### set_param / set_parameter

Batch form:

```json
{
  "type": "control",
  "payload": {
    "command": "set_param",
    "parameters": {
      "report_interval_ms": 1000,
      "flow_input_inverted": false
    }
  }
}
```

Single form:

```json
{
  "type": "control",
  "payload": {
    "command": "set_parameter",
    "parameters": {
      "name": "report_interval_ms",
      "value": 1000
    }
  }
}
```

Supported parameter keys:

- `report_interval_ms` (integer, clamped 200..10000)
- `flow_input_inverted` (bool)
- `reset_total_liters` (bool or `1` to zero counters)

## Notes for integration

- Calibration is fixed in firmware (`pulses_per_liter = 70.01`) and currently exposed as read-only status metadata.
- Unknown `set_param` keys are ignored.
- For `set_param` / `set_parameter`, if at least one parameter is applied, firmware persists settings and immediately emits a `status` response.

## Common control examples

Request immediate status:

```json
{
  "type": "control",
  "payload": {
    "command": "status_request"
  }
}
```

Reset total volume/counters:

```json
{
  "type": "control",
  "payload": {
    "command": "set_param",
    "parameters": {
      "reset_total_liters": true
    }
  }
}
```
