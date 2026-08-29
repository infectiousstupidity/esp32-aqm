#pragma once

#include <Arduino.h>

// Per-sensor health, owned by sensors.cpp and exposed through the snapshot.
struct SensorHealth
{
  bool running = false;           // initialized and expected to be running
  bool hasSample = false;         // at least one successful sample since boot
  uint32_t lastSuccessMs = 0;     // millis() of the last successful sample
  uint32_t readErrors = 0;        // cumulative read errors since boot
  uint32_t consecutiveErrors = 0; // consecutive read errors
};

struct SensorSnapshot
{
  // Last good values. Retained across invalid cycles; the validity flags
  // say whether the value is from the current measurement cycle.
  float temperature = NAN;
  float humidity = NAN;
  float pressure = NAN; // ambient pressure in pascals
  uint16_t co2 = 0;

  // SPS30 particulate aggregate: arithmetic mean of the valid 1-second
  // readings collected during the last 30-second measurement window.
  float pm10 = NAN;                // PM1.0 mass concentration, ug/m3
  float pm25 = NAN;                // PM2.5 mass concentration, ug/m3
  float pm40 = NAN;                // PM4.0 mass concentration, ug/m3
  float pm100 = NAN;               // PM10.0 mass concentration, ug/m3
  float typicalParticleSize = NAN; // typical particle size, um

  bool temperatureValid = false;
  bool humidityValid = false;
  bool pressureValid = false;
  bool co2Valid = false;
  bool pmValid = false; // all PM channels share one aggregate validity

  SensorHealth bme280;
  SensorHealth scd41;
  SensorHealth sps30;
};

void beginSensors();
void updateSensors();
// Lightweight per-loop servicing: collects SPS30 readings at a 1-second
// cadence for the 30-second aggregate. Safe to call every loop iteration.
void serviceSensors();
const SensorSnapshot &getSensorSnapshot();
