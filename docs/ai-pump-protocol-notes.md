# AI Pump Protocol Notes

HydraBridge includes experimental local BLE support for AquaIllumination pumps that use the Mobius-style pump scene path, starting with Orbit-class devices.

This implementation is an independently written interoperability layer. It does not contain vendor firmware, vendor application code, copyrighted assets, or cloud service integrations.

## Current Scope

The first pump implementation supports:

- Discovery classification for Hydra lights and Orbit 2 / Orbit 4 pumps only. Other BLE advertisements are ignored by the web scan result buffer.
- A separate NVS-backed pump registry with up to 4 registered pumps.
- Manual pump commands from the web UI.
- Local pump schedules with fixed-time, sunrise, and sunset triggers.
- BLE write dispatch through the existing NimBLE command worker.

The web UI and schedule engine expose the Orbit-supported command modes:

| Mode | Numeric value | Notes |
|---|---:|---|
| Constant | `1` | Runs the pump at the requested speed percentage. |
| Random | `15` | Uses min speed, max speed, and variance percentages. |
| Pulse | `16` | Uses max speed plus on/off pulse timing in milliseconds. |
| Feed | `13` | Sends the feed-mode primitive with the requested speed percentage. |

Speed is accepted as `0..100` percent and encoded as tenths of a percent in the pump primitive.

The lower-level protocol builder also understands the Mobius pump mode IDs found during interoperability work:

| Mode | Numeric value |
|---|---:|
| Lagoon | `2` |
| Reef Crest | `3` |
| Nutrient Transport | `4` |
| Tidal Swell | `5` |
| Short Pulse | `6` |
| Gyre | `7` |
| Transition | `8` |
| Expanding Pulse | `9` |
| Sync | `10` |
| EcoSmart Back | `12` |
| Battery Backup | `14` |

These broader modes are not exposed for Orbit schedules. `Sync` additionally requires an explicit 8-byte master identity, so it is rejected unless the caller provides that value.

## Payload Shape

Pump commands use the experimental `LiveDemoSceneNero` SetC2Attr path:

| Field | Value |
|---|---|
| Attribute ID | `0x0194` |
| Primitive type | `0x02` |
| Scene ID | `1` |
| Scene name bytes | `16`, zero-filled |
| Pump primitive bytes | `13` |
| Full SetC2Attr payload bytes | `42` |

The command builder lives in `components/ai_pump_protocol/` and is covered by host tests in `host_tests/tests/test_ai_pump_protocol.c`.

## Scheduling

Pump schedules are stored separately from lighting schedules and support up to 12 entries. Each schedule targets one registered pump and defines:

- Start trigger: fixed time, sunrise offset, or sunset offset.
- End trigger: fixed time, sunrise offset, or sunset offset.
- Active mode, speed, min speed, variance, pulse on time, and pulse off time.
- End mode, speed, min speed, variance, pulse on time, and pulse off time.

The schedule engine runs locally on the ESP32 and queues pump commands through `command_queue`, so pump scheduling continues without cloud access once time and sun settings are available.

## Validation Status

Host tests verify registry behavior, payload construction, schedule trigger math, config defaults, and queue behavior. Firmware builds clean for the ESP32-S3 target.

Real pump behavior should still be treated as experimental until validated on physical Orbit 2 / Orbit 4 hardware. Keep livestock safety in mind: monitor first runs, use conservative speeds, and keep vendor controls available as a fallback.
