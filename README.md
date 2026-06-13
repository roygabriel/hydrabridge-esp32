# Hydra 64HD BLE Controller

ESP32 controller for Aqua Illumination Hydra 64HD lights using the reverse-engineered myAI BLE protocol.

Primary local automation path is RS485 / Modbus RTU for a Productivity 2000 P2000 CODESYS CPU. MQTT, WiFi, Home Assistant discovery, and the web UI are optional richer interfaces.

## Layout

- `main/` - ESP-IDF application entrypoint.
- `components/` - controller modules matching the implementation plan.
- `docs/` - protocol notes and implementation plan.
- `reference/decompiled/` - local decompiled APK reference; ignored by git.

## Build

Install ESP-IDF, then run:

```bash
idf.py set-target esp32
idf.py build
```

## References

- `docs/myai-ble-reverse-engineering.md`
- `docs/esp32-hydra64hd-controller-plan.md`
