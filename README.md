# ESP32 Air Quality Monitor

A compact indoor air-quality monitor built around an ESP32 and a 2.13-inch
black-and-white e-paper display.

The monitor samples carbon dioxide, particulate matter, temperature, humidity,
and pressure. Current CO2 and PM2.5 readings are shown on the display alongside
short-term history, while the complete measurement set is exposed as
Prometheus metrics over Wi-Fi.

## Hardware

- ESP32 development board
- 2.13-inch black-and-white e-paper display supported by GxEPD2
- Bosch BME280 temperature, humidity, and pressure sensor
- Sensirion SCD41 CO2 sensor
- Sensirion SPS30 particulate matter sensor

### Connections

The sensors share the ESP32's I2C bus.

| Component | Signal | ESP32 pin |
| --- | --- | ---: |
| BME280, SCD41, SPS30 | SDA | 21 |
| BME280, SCD41, SPS30 | SCL | 22 |
| E-paper display | MOSI | 23 |
| E-paper display | SCK | 18 |
| E-paper display | CS | 5 |
| E-paper display | DC | 19 |
| E-paper display | RST | 2 |
| E-paper display | BUSY | 4 |

The configured I2C addresses are `0x76` for the BME280, `0x62` for the SCD41,
and `0x69` for the SPS30.

## Setup

This project uses [PlatformIO](https://platformio.org/) with the Arduino
framework. Dependencies are pinned in `platformio.ini` and installed
automatically during the first build.

1. Copy the Wi-Fi configuration template:

   ```sh
   cp include/secrets.example.h include/secrets.h
   ```

2. Add your network credentials to `include/secrets.h`.

3. Connect the ESP32 and build, upload, then open the serial monitor:

   ```sh
   pio run
   pio run --target upload
   pio device monitor
   ```

The serial monitor runs at `115200` baud. Local credentials are ignored by
Git and should not be committed.

## Usage

Measurements are committed every 30 seconds and the display refreshes every
60 seconds. The display presents CO2 and PM2.5 values with a 30-minute history,
plus the current temperature and relative humidity.

Once connected to Wi-Fi, the device advertises itself over mDNS using a unique
hostname derived from its MAC address:

```text
http://air-monitor-xxxxxx.local/
http://air-monitor-xxxxxx.local/metrics
```

The `/metrics` endpoint uses the Prometheus text exposition format and includes
sensor readings, sensor health, uptime, reset information, heap usage, and
Wi-Fi status. The assigned IP address and exact mDNS hostname are printed to
the serial monitor after connection.

## Notes

- [SCD41 calibration](docs/CALIBRATION.md)
- [Watchdog behavior and validation](docs/WATCHDOG.md)
