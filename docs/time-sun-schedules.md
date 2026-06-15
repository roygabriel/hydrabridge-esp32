# Time, Sun Events, and Lighting Schedules

HydraBridge ESP32 can run local lighting schedules after the controller has a valid clock.

## Time Sync

Time sync is configured from the web UI under **Settings > Time Sync**.

- Time sync is enabled by default.
- The default SNTP server is `time.nist.gov`.
- You can set a custom SNTP/NTP hostname.
- The timezone is stored as a POSIX `TZ` string and applied locally on the ESP32.
- If time sync fails, manual BLE, MQTT, RS485, and web control continue to work.

The current time status is available from:

```text
GET /api/config/time
GET /api/status
```

## Sun Events

Sunrise and sunset are computed locally from latitude and longitude. HydraBridge does not call a remote sunrise/sunset API or send location data to a third party.

Configure sun events from **Settings > Sun Events**:

- Enable or disable sunrise/sunset triggers.
- Set a location label.
- Set latitude and longitude in decimal degrees.

The controller computes today's local sunrise and sunset after the clock is valid. Status is available from:

```text
GET /api/config/sun
GET /api/status
```

## Lighting Schedules

Schedules are configured from the **Schedules** tab.

Each schedule block includes:

- Target: one registered light or one light group.
- Profile: any built-in or saved profile.
- Day intensity percentage.
- End intensity percentage.
- Start trigger: fixed time, sunrise, or sunset.
- End trigger: fixed time, sunrise, or sunset.
- Optional start/end offsets for sun triggers.
- Ramp-up and ramp-down durations.
- Enabled/disabled state.

Schedule commands are submitted through the same command engine used by the web UI, MQTT, and RS485 paths. Group targets fan out through the existing group support.

Schedules persist in NVS and are reloaded after reboot. After time sync becomes valid, the scheduler evaluates the current block and applies the correct current intensity once.

Schedule API:

```text
GET /api/schedules
POST /api/schedules
DELETE /api/schedules/<schedule_id>
```

Example `POST /api/schedules` body:

```json
{
  "enabled": true,
  "name": "Reef day",
  "target_type": 1,
  "target_id": "grp-displaytank",
  "profile_name": "AB Plus",
  "intensity_percent": 70,
  "end_intensity_percent": 0,
  "start_trigger": 1,
  "end_trigger": 2,
  "start_offset_min": 30,
  "end_offset_min": -45,
  "ramp_up_min": 90,
  "ramp_down_min": 120
}
```

Trigger values:

| Value | Trigger |
|---:|---|
| 0 | Fixed local time |
| 1 | Sunrise |
| 2 | Sunset |

Target values:

| Value | Target |
|---:|---|
| 0 | Light |
| 1 | Group |
