# SCD41 calibration notes

## Automatic self-calibration (ASC)

The SCD41's automatic self-calibration (ASC) is enabled by factory default,
and this firmware keeps it enabled explicitly at every initialization
(`SCD41_AUTOMATIC_SELF_CALIBRATION` in `src/sensors.cpp`). The setting is
applied to the sensor's volatile (RAM) configuration only; the firmware never
issues `persist_settings`, so the sensor EEPROM is not written at startup.

ASC relies on an assumption: the sensor is exposed to outdoor fresh air at
about 400 ppm CO2 at least once per week of accumulated operation, for at
least 4 hours without interruption, while running in one of its measurement
modes. If the device is installed where that never happens (for example, a
sealed space that is never ventilated with outside air), the ASC baseline can
drift and CO2 readings will be biased.

## Manual forced recalibration (FRC)

A forced recalibration pins the CO2 baseline to a known reference
concentration. Do this only with a trustworthy reference, for example:

- fresh outdoor air on a day with low traffic and pollution (typically
  400-450 ppm), or
- a calibrated reference gas cylinder.

**Do not treat arbitrary indoor air as a 400 ppm reference.** Indoor air is
usually well above outdoor background: occupants, cooking, candles, and poor
ventilation all raise CO2. Forcing the baseline to 400 ppm against such air
will bias every later reading low.

Procedure (requires I2C access to the SCD41, e.g. a development host or a
temporary firmware build; this firmware intentionally has no HTTP
calibration endpoint):

1. Run the SCD41 in its normal operating mode (low-power periodic, 30 s) for
   at least 3 minutes in the environment whose CO2 concentration you will use
   as the reference. The air must be homogeneous and constant during this
   time.
2. Send `stop_periodic_measurement` to return the sensor to idle.
3. Send `perform_forced_recalibration` with the target CO2 concentration in
   ppm (e.g. 400 for verified fresh outdoor air). A non-zero return value
   means the FRC failed (for example, the sensor was not operated before the
   command).
4. Restart the normal periodic measurement mode.

FRC history is stored by the sensor in its own calibration EEPROM; the
firmware does not need to persist anything.
