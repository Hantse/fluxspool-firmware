# fluxspool-firmware

## Fluxspool V1.0 PCB pinout

The production PCB uses an ESP32-S3-WROOM-1-N8 module.

| Function | ESP32-S3 GPIO |
| --- | --- |
| HX711 DOUT / DT | GPIO9 |
| HX711 SCK / PD_SCK | GPIO10 |
| I2C SDA | GPIO14 |
| I2C SCL | GPIO21 |
| MFRC522 RST | GPIO38 |
| MFRC522 MISO | GPIO39 |
| MFRC522 MOSI | GPIO40 |
| MFRC522 SCK | GPIO41 |
| MFRC522 SDA / SS | GPIO42 |
| USB D- | GPIO19 |
| USB D+ | GPIO20 |
| BOOT | GPIO0 |

## Standalone V1 scan behavior

In the production V1 standalone firmware, the `Scan` command reads the local
RFID reader, HX711 scale, and color sensor. Each sensor is retried 3 times by
default before returning a result.

The command result includes `ok=true` only when all three readings succeeded.
If one sensor is missing or times out, the response still includes any available
readings plus `partial=true`, `missing`, `errors`, and per-sensor `attempts`.
