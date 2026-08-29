/***********************************************************************
  2.13" B/W e-paper air quality monitor
  ESP32 + GxEPD2 + BME280 + SCD41 + SPS30
***********************************************************************/

#include <Arduino.h>

#include "dashboard.h"
#include "device_info.h"
#include "network.h"
#include "sensors.h"
#include "watchdog.h"

namespace
{
// Measurement and history are committed on this cadence.
constexpr uint32_t MEASUREMENT_INTERVAL_MS = 30000;
// The e-paper display refreshes on its own, slower cadence.
constexpr uint32_t DISPLAY_INTERVAL_MS = 60000;

uint32_t last_measurement_ms = 0;
uint32_t last_display_ms = 0;

void commitMeasurement()
{
  updateSensors();

  // Record the sample into history; this does not refresh the display.
  recordDashboardSample(getSensorSnapshot());
}
} // namespace

void setup()
{
  Serial.begin(115200);

  // Boot diagnostics (version, reset reason, persisted boot count) first,
  // so they appear before any sensor or network activity.
  beginDeviceInfo();

  // Arm the main-loop watchdog before any potentially blocking work. It is
  // fed from loop() only; a stuck loop stops feeding and resets the chip.
  beginWatchdog();

  beginSensors();
  beginDashboard();

  // Initial measurement and initial full display refresh.
  commitMeasurement();
  drawDashboard(getSensorSnapshot());

  beginNetwork();

  last_measurement_ms = millis();
  last_display_ms = millis();

  Serial.println("Setup complete");
}

void loop()
{
  // Feed the main-loop watchdog first, before any potentially blocking work
  // (sensor reads, e-paper refresh). Fed from the cooperative loop only.
  feedWatchdog();

  serviceNetwork();

  // Track the min-free-heap watermark once per loop iteration.
  updateMinFreeHeap();

  // Collect SPS30 samples at a 1-second cadence for the 30-second
  // aggregate; lightweight, gated internally.
  serviceSensors();

  const uint32_t now = millis();

  if (now - last_measurement_ms >= MEASUREMENT_INTERVAL_MS)
  {
    last_measurement_ms = now;

    // Sensor reads and history commits are synchronous. HTTP requests
    // arriving during this update are handled after it finishes.
    commitMeasurement();
  }

  if (now - last_display_ms >= DISPLAY_INTERVAL_MS)
  {
    last_display_ms = now;

    // The e-paper refresh is synchronous and blocks the cooperative loop
    // (longer for a full refresh). HTTP requests arriving during the
    // refresh are handled after it finishes.
    drawDashboard(getSensorSnapshot());
  }

  // Yield to the ESP32 networking stack.
  delay(2);
}
