#include "sensors.h"

#include <Adafruit_BME280.h>
#include <SensirionCore.h>
#include <SensirionI2cScd4x.h>
#include <SensirionI2cSps30.h>
#include <Wire.h>

namespace
{
constexpr uint8_t I2C_SDA = 21;
constexpr uint8_t I2C_SCL = 22;
constexpr uint8_t SCD41_I2C_ADDR = 0x62;
constexpr uint8_t BME280_I2C_ADDR = 0x76;
constexpr uint32_t SPS30_WARMUP_MS = 30000;
// The SPS30 publishes a new measurement every second in measurement mode;
// collect at most one reading per second.
constexpr uint32_t SPS30_SAMPLE_INTERVAL_MS = 1000;
// Reinitialize (soft reset) only after this many consecutive 1-second
// cycles without a valid read; a single transient miss must not restart
// the sensor.
constexpr uint32_t SPS30_REINIT_AFTER_CONSECUTIVE_MISSES = 10;

// Preserve the sensor's existing automatic fan-cleaning interval unless a
// deliberate project setting overrides it. While the override is disabled
// the interval is only read and logged, never written.
constexpr bool SPS30_OVERRIDE_CLEANING_INTERVAL = false;
constexpr uint32_t SPS30_CLEANING_INTERVAL_S = 604800; // 1 week (factory default)

// SPS30 mass concentration sanity range, ug/m3.
constexpr float SPS30_PM_MAX_UG_M3 = 5000.0f;
// SPS30 typical particle size sanity range, um (datasheet: 0.5-64 um;
// the check also accepts 0.0, which is the safer bound).
constexpr float SPS30_TYPICAL_SIZE_MAX_UM = 64.0f;

// SCD41 operating mode. Low-power periodic (30 s signal interval) matches the
// project's 30 s CO2 consumption cadence. Set to false to restore the
// standard 5 s periodic mode without restructuring this module.
constexpr bool SCD41_LOW_POWER_MODE = true;

// Automatic self-calibration (ASC) is enabled by factory default; keep it
// enabled unless a deliberate project setting says otherwise. Applied to the
// sensor's volatile configuration at every init; never persisted to the
// sensor EEPROM by this firmware.
constexpr bool SCD41_AUTOMATIC_SELF_CALIBRATION = true;

// SCD41 setAmbientPressure() accepts 70000-120000 Pa (library-documented).
// Pressures outside this range are not passed to the SCD41; the sensor then
// keeps its own compensation.
constexpr float SCD41_PRESSURE_MIN_PA = 70000.0f;
constexpr float SCD41_PRESSURE_MAX_PA = 120000.0f;

// BME280 measurement range is 300-1100 hPa (30000-110000 Pa).
constexpr float BME280_PRESSURE_MIN_HPA = 300.0f;
constexpr float BME280_PRESSURE_MAX_HPA = 1100.0f;

Adafruit_BME280 bme;
SensirionI2cScd4x scd4x;
SensirionI2cSps30 sps30;

SensorSnapshot current_snapshot;

uint32_t sps30_started_ms = 0;
uint32_t sps30_last_sample_ms = 0;
uint32_t sps30_consecutive_misses = 0;

struct Sps30Sample
{
  float pm10;
  float pm25;
  float pm40;
  float pm100;
  float typicalParticleSize;
};

// Valid 1-second readings collected since the last committed window.
struct Sps30Accumulator
{
  float sumPm10 = 0.0f;
  float sumPm25 = 0.0f;
  float sumPm40 = 0.0f;
  float sumPm100 = 0.0f;
  float sumTypicalSize = 0.0f;
  uint32_t count = 0;

  void reset()
  {
    sumPm10 = 0.0f;
    sumPm25 = 0.0f;
    sumPm40 = 0.0f;
    sumPm100 = 0.0f;
    sumTypicalSize = 0.0f;
    count = 0;
  }
};

Sps30Accumulator sps30_accumulator;

enum class SensorReadResult
{
  Success,
  NoData,
  Error,
};

void printSensirionError(const char *operation, int16_t error)
{
  char errorMessage[64];
  errorToString(error, errorMessage, sizeof errorMessage);

  Serial.print(operation);
  Serial.print(": ");
  Serial.println(errorMessage);
}

bool initializeBME280()
{
  if (bme.begin(BME280_I2C_ADDR))
  {
    return true;
  }

  Serial.println("BME280 not found");
  return false;
}

bool initializeSCD41()
{
  scd4x.begin(Wire, SCD41_I2C_ADDR);

  // Official Sensirion lifecycle: establish a known idle state first.
  int16_t error = scd4x.stopPeriodicMeasurement();

  if (error != 0)
  {
    printSensirionError("SCD41 stopPeriodicMeasurement error", error);
    return false;
  }

  // Identify the sensor where supported (idle only). A failure here is not
  // fatal: log it and continue, the sensor may still operate.
  uint64_t serialNumber = 0;
  error = scd4x.getSerialNumber(serialNumber);

  if (error != 0)
  {
    printSensirionError("SCD41 getSerialNumber error", error);
  }

  // Configure ASC deliberately (idle only). The setting is volatile (RAM),
  // so it is applied at every init and the sensor EEPROM is never written.
  error = scd4x.setAutomaticSelfCalibrationEnabled(
      SCD41_AUTOMATIC_SELF_CALIBRATION ? 1 : 0);

  if (error != 0)
  {
    printSensirionError(
        "SCD41 setAutomaticSelfCalibrationEnabled error", error);
    return false;
  }

  // Then start the configured periodic measurement mode (idle only).
  if (SCD41_LOW_POWER_MODE)
  {
    error = scd4x.startLowPowerPeriodicMeasurement();
  }
  else
  {
    error = scd4x.startPeriodicMeasurement();
  }

  if (error != 0)
  {
    printSensirionError("SCD41 startPeriodicMeasurement error", error);
    return false;
  }

  // Log the identity and selected settings once at successful init. The
  // serial number is for local diagnostics only; it is never exposed as a
  // metric (high cardinality).
  Serial.printf(
      "SCD41 initialized: serial=0x%012llX, mode=%s, ASC=%s\n",
      (unsigned long long)serialNumber,
      SCD41_LOW_POWER_MODE ? "low-power periodic (30 s)"
                           : "periodic (5 s)",
      SCD41_AUTOMATIC_SELF_CALIBRATION ? "enabled" : "disabled");

  return true;
}

bool initializeSPS30()
{
  sps30.begin(Wire, SPS30_I2C_ADDR_69);

  int16_t error = sps30.stopMeasurement();

  if (error != 0)
  {
    printSensirionError("SPS30 stopMeasurement error", error);
    return false;
  }

  // Identify the sensor and report its maintenance configuration once per
  // initialization (idle mode). Failures here are not fatal: log and
  // continue, the sensor may still operate.
  int8_t productType[9] = {0};
  error = sps30.readProductType(productType, 8);

  if (error != 0)
  {
    printSensirionError("SPS30 readProductType error", error);
  }

  int8_t serialNumber[32] = {0};
  error = sps30.readSerialNumber(serialNumber, sizeof serialNumber);

  if (error != 0)
  {
    printSensirionError("SPS30 readSerialNumber error", error);
  }

  serialNumber[sizeof serialNumber - 1] = 0;

  uint8_t fwMajor = 0;
  uint8_t fwMinor = 0;
  error = sps30.readFirmwareVersion(fwMajor, fwMinor);

  if (error != 0)
  {
    printSensirionError("SPS30 readFirmwareVersion error", error);
  }

  uint32_t cleaningIntervalS = 0;
  error = sps30.readAutoCleaningInterval(cleaningIntervalS);

  if (error != 0)
  {
    printSensirionError("SPS30 readAutoCleaningInterval error", error);
  }
  else if (SPS30_OVERRIDE_CLEANING_INTERVAL)
  {
    // Deliberate project override; the interval is written only in that
    // case, never on every boot.
    const int16_t writeError =
        sps30.writeAutoCleaningInterval(SPS30_CLEANING_INTERVAL_S);

    if (writeError != 0)
    {
      printSensirionError(
          "SPS30 writeAutoCleaningInterval error", writeError);
    }

    cleaningIntervalS = SPS30_CLEANING_INTERVAL_S;
  }

  error = sps30.startMeasurement(
      SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT);

  if (error != 0)
  {
    printSensirionError("SPS30 startMeasurement error", error);
    return false;
  }

  Serial.printf(
      "SPS30 initialized: product=%s, serial=%s, firmware=%u.%u, "
      "fan cleaning every %lu s\n",
      (const char *)productType,
      (const char *)serialNumber,
      fwMajor,
      fwMinor,
      (unsigned long)cleaningIntervalS);

  // A new measurement session needs warmup again and starts a fresh
  // aggregation window. The last good PM values are retained but stay
  // invalid until warmup completes and a new window is committed.
  sps30_accumulator.reset();
  sps30_consecutive_misses = 0;
  current_snapshot.pmValid = false;
  sps30_started_ms = millis();
  return true;
}

SensorReadResult readBME280(
    float &temperatureOut,
    float &humidityOut,
    float &pressureOut)
{
  const float temperature = bme.readTemperature();
  const float humidity = bme.readHumidity();
  const float pressureHpa = bme.readPressure();

  // A non-finite value means the I2C read itself failed.
  if (!isfinite(temperature) ||
      !isfinite(humidity) ||
      !isfinite(pressureHpa))
  {
    return SensorReadResult::Error;
  }

  // The sensor answered but the data is unusable.
  if (temperature < -40.0f ||
      temperature > 85.0f ||
      humidity < 0.0f ||
      humidity > 100.0f ||
      pressureHpa < BME280_PRESSURE_MIN_HPA ||
      pressureHpa > BME280_PRESSURE_MAX_HPA)
  {
    Serial.println("BME280 reading rejected");
    return SensorReadResult::NoData;
  }

  temperatureOut = temperature;
  humidityOut = humidity;
  pressureOut = pressureHpa * 100.0f; // hPa -> Pa
  return SensorReadResult::Success;
}

SensorReadResult readSCD41(uint16_t &co2Out)
{
  bool isDataReady = false;
  int16_t error = scd4x.getDataReadyStatus(isDataReady);

  if (error != 0)
  {
    printSensirionError("SCD41 getDataReadyStatus error", error);
    return SensorReadResult::Error;
  }

  if (!isDataReady)
  {
    Serial.println("SCD41 data not ready");
    return SensorReadResult::NoData;
  }

  uint16_t co2 = 0;
  float sensorTemperature = 0.0f;
  float sensorHumidity = 0.0f;

  error = scd4x.readMeasurement(
      co2,
      sensorTemperature,
      sensorHumidity);

  if (error != 0)
  {
    printSensirionError("SCD41 readMeasurement error", error);
    return SensorReadResult::Error;
  }

  if (co2 == 0)
  {
    Serial.println("SCD41 no usable data: CO2 is 0");
    return SensorReadResult::NoData;
  }

  co2Out = co2;
  return SensorReadResult::Success;
}

SensorReadResult readSPS30(Sps30Sample &sampleOut)
{
  uint16_t dataReadyFlag = 0;
  int16_t error = sps30.readDataReadyFlag(dataReadyFlag);

  if (error != 0)
  {
    printSensirionError("SPS30 readDataReadyFlag error", error);
    return SensorReadResult::Error;
  }

  if (dataReadyFlag == 0)
  {
    Serial.println("SPS30 data not ready");
    return SensorReadResult::NoData;
  }

  float mc1p0 = 0;
  float mc2p5 = 0;
  float mc4p0 = 0;
  float mc10p0 = 0;
  float nc0p5 = 0;
  float nc1p0 = 0;
  float nc2p5 = 0;
  float nc4p0 = 0;
  float nc10p0 = 0;
  float typicalParticleSize = 0;

  error = sps30.readMeasurementValuesFloat(
      mc1p0,
      mc2p5,
      mc4p0,
      mc10p0,
      nc0p5,
      nc1p0,
      nc2p5,
      nc4p0,
      nc10p0,
      typicalParticleSize);

  if (error != 0)
  {
    printSensirionError("SPS30 readMeasurementValuesFloat error", error);
    return SensorReadResult::Error;
  }

  // Reject non-finite or out-of-range readings on any published channel.
  if (!isfinite(mc1p0) ||
      !isfinite(mc2p5) ||
      !isfinite(mc4p0) ||
      !isfinite(mc10p0) ||
      !isfinite(typicalParticleSize) ||
      mc1p0 < 0.0f ||
      mc1p0 > SPS30_PM_MAX_UG_M3 ||
      mc2p5 < 0.0f ||
      mc2p5 > SPS30_PM_MAX_UG_M3 ||
      mc4p0 < 0.0f ||
      mc4p0 > SPS30_PM_MAX_UG_M3 ||
      mc10p0 < 0.0f ||
      mc10p0 > SPS30_PM_MAX_UG_M3 ||
      typicalParticleSize < 0.0f ||
      typicalParticleSize > SPS30_TYPICAL_SIZE_MAX_UM)
  {
    Serial.printf(
        "SPS30 no usable data: PM1.0=%.2f PM2.5=%.2f PM4.0=%.2f "
        "PM10=%.2f ug/m3, size=%.2f um\n",
        mc1p0,
        mc2p5,
        mc4p0,
        mc10p0,
        typicalParticleSize);
    return SensorReadResult::NoData;
  }

  sampleOut.pm10 = mc1p0;
  sampleOut.pm25 = mc2p5;
  sampleOut.pm40 = mc4p0;
  sampleOut.pm100 = mc10p0;
  sampleOut.typicalParticleSize = typicalParticleSize;
  return SensorReadResult::Success;
}
} // namespace

void beginSensors()
{
  Wire.begin(I2C_SDA, I2C_SCL);

  // SPS30 standard-mode I2C.
  Wire.setClock(100000);
}

void markSampleSuccess(SensorHealth &health)
{
  health.hasSample = true;
  health.lastSuccessMs = millis();
  health.consecutiveErrors = 0;
}

void markReadError(SensorHealth &health)
{
  health.readErrors++;
  health.consecutiveErrors++;

  // Re-initialize on the next cycle.
  health.running = false;
}

void updateBME280()
{
  SensorHealth &health = current_snapshot.bme280;

  if (!health.running)
  {
    health.running = initializeBME280();
    return;
  }

  float temperature = NAN;
  float humidity = NAN;
  float pressure = NAN;

  const SensorReadResult result =
      readBME280(temperature, humidity, pressure);

  if (result != SensorReadResult::Success)
  {
    // Keep the last good values; only the validity flags change.
    current_snapshot.temperatureValid = false;
    current_snapshot.humidityValid = false;
    current_snapshot.pressureValid = false;

    if (result == SensorReadResult::Error)
    {
      markReadError(health);
    }

    return;
  }

  current_snapshot.temperature = temperature;
  current_snapshot.humidity = humidity;
  current_snapshot.pressure = pressure;
  current_snapshot.temperatureValid = true;
  current_snapshot.humidityValid = true;
  current_snapshot.pressureValid = true;
  markSampleSuccess(health);
}

void updateSCD41()
{
  SensorHealth &health = current_snapshot.scd41;

  if (!health.running)
  {
    health.running = initializeSCD41();

    // Do not read immediately after (re)starting periodic measurement.
    // The last good CO2 value is retained but stays invalid.
    return;
  }

  uint16_t co2 = 0;

  // Supply fresh ambient pressure while periodic measurement is active.
  // Only valid, in-range values are passed; a missing or out-of-range
  // pressure never blocks CO2 measurement, the SCD41 keeps its own
  // compensation in that case.
  if (current_snapshot.pressureValid &&
      current_snapshot.pressure >= SCD41_PRESSURE_MIN_PA &&
      current_snapshot.pressure <= SCD41_PRESSURE_MAX_PA)
  {
    const int16_t pressureError =
        scd4x.setAmbientPressure((uint32_t)current_snapshot.pressure);

    if (pressureError != 0)
    {
      printSensirionError("SCD41 setAmbientPressure error", pressureError);
    }
  }

  const SensorReadResult result = readSCD41(co2);

  if (result != SensorReadResult::Success)
  {
    current_snapshot.co2Valid = false;

    if (result == SensorReadResult::Error)
    {
      markReadError(health);
    }

    return;
  }

  current_snapshot.co2 = co2;
  current_snapshot.co2Valid = true;
  markSampleSuccess(health);
}

void updateSPS30()
{
  SensorHealth &health = current_snapshot.sps30;

  if (!health.running)
  {
    health.running = initializeSPS30();

    // A new measurement session needs warmup again. The last good PM
    // values are retained but stay invalid until warmup completes.
    return;
  }

  const bool warmedUp =
      millis() - sps30_started_ms >= SPS30_WARMUP_MS;

  if (!warmedUp)
  {
    return;
  }

  // Commit the arithmetic mean of the valid readings collected during the
  // last window. With no valid readings the aggregate stays invalid and
  // the last good values are retained.
  if (sps30_accumulator.count > 0)
  {
    const float n = (float)sps30_accumulator.count;

    current_snapshot.pm10 = sps30_accumulator.sumPm10 / n;
    current_snapshot.pm25 = sps30_accumulator.sumPm25 / n;
    current_snapshot.pm40 = sps30_accumulator.sumPm40 / n;
    current_snapshot.pm100 = sps30_accumulator.sumPm100 / n;
    current_snapshot.typicalParticleSize =
        sps30_accumulator.sumTypicalSize / n;
    current_snapshot.pmValid = true;

    Serial.printf(
        "SPS30 window committed: %lu samples\n",
        (unsigned long)sps30_accumulator.count);

    markSampleSuccess(health);
  }
  else
  {
    current_snapshot.pmValid = false;
  }

  // Start the next aggregation window.
  sps30_accumulator.reset();
}

void recoverSPS30()
{
  Serial.printf(
      "SPS30 recovery: soft reset after %lu consecutive missed reads\n",
      (unsigned long)sps30_consecutive_misses);

  const int16_t resetError = sps30.deviceReset();

  if (resetError != 0)
  {
    printSensirionError("SPS30 deviceReset error", resetError);
  }

  // Re-initialization restarts the warmup and resets the accumulator.
  current_snapshot.sps30.running = initializeSPS30();
  current_snapshot.sps30.consecutiveErrors = 0;
}

void logSensorLine()
{
  char co2[8] = "N/A";
  char pm25[8] = "N/A";
  char temperature[8] = "N/A";
  char humidity[8] = "N/A";
  char pressure[12] = "N/A";

  if (current_snapshot.co2Valid)
  {
    snprintf(co2, sizeof co2, "%u", current_snapshot.co2);
  }

  if (current_snapshot.pmValid)
  {
    snprintf(pm25, sizeof pm25, "%.1f", current_snapshot.pm25);
  }

  if (current_snapshot.temperatureValid)
  {
    snprintf(temperature, sizeof temperature, "%.1f", current_snapshot.temperature);
  }

  if (current_snapshot.humidityValid)
  {
    snprintf(humidity, sizeof humidity, "%.0f", current_snapshot.humidity);
  }

  if (current_snapshot.pressureValid)
  {
    snprintf(pressure, sizeof pressure, "%.0f", current_snapshot.pressure);
  }

  Serial.printf(
      "CO2=%s ppm | PM2.5=%s ug/m3 | "
      "Temp=%s C | RH=%s%% | Pressure=%s Pa\n",
      co2,
      pm25,
      temperature,
      humidity,
      pressure);
}

void updateSensors()
{
  updateBME280();
  updateSCD41();
  updateSPS30();

  logSensorLine();
}

void serviceSensors()
{
  SensorHealth &health = current_snapshot.sps30;

  // Initialization and retry are handled by updateSensors() on the
  // 30-second boundary; this path only collects samples from a running
  // sensor.
  if (!health.running)
  {
    return;
  }

  const uint32_t now = millis();

  // No samples during warmup after (re)start.
  if (now - sps30_started_ms < SPS30_WARMUP_MS)
  {
    return;
  }

  if (now - sps30_last_sample_ms < SPS30_SAMPLE_INTERVAL_MS)
  {
    return;
  }

  sps30_last_sample_ms = now;

  Sps30Sample sample;
  const SensorReadResult result = readSPS30(sample);

  if (result == SensorReadResult::Success)
  {
    sps30_accumulator.sumPm10 += sample.pm10;
    sps30_accumulator.sumPm25 += sample.pm25;
    sps30_accumulator.sumPm40 += sample.pm40;
    sps30_accumulator.sumPm100 += sample.pm100;
    sps30_accumulator.sumTypicalSize += sample.typicalParticleSize;
    sps30_accumulator.count++;

    sps30_consecutive_misses = 0;
    health.consecutiveErrors = 0;
    return;
  }

  // A not-ready result is not a read error; only transport errors count.
  if (result == SensorReadResult::Error)
  {
    health.readErrors++;
    health.consecutiveErrors++;
  }

  sps30_consecutive_misses++;

  if (sps30_consecutive_misses >= SPS30_REINIT_AFTER_CONSECUTIVE_MISSES)
  {
    recoverSPS30();
  }
}

const SensorSnapshot &getSensorSnapshot()
{
  return current_snapshot;
}
