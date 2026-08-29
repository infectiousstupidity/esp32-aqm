#include "watchdog.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

namespace
{
// Timeout in seconds. Must exceed the worst-case blocking operation in the
// cooperative loop: a full e-paper refresh (~3600 ms) plus the worst expected
// sensor transaction. 20 s leaves wide margin above that, so normal full
// refreshes never trip the watchdog, while a genuinely stuck loop resets the
// chip within 20 s.
constexpr uint32_t WATCHDOG_TIMEOUT_S = 20;

// Compile-time test hook for hardware validation only. When true, the feed is
// withheld so the watchdog fires after WATCHDOG_TIMEOUT_S, letting you confirm
// the reset and the "watchdog" reset-reason telemetry. This is a build-time
// constant: it cannot be toggled remotely or at runtime, so it cannot be
// triggered in production. It MUST be set back to false and the firmware
// rebuilt before any production flash.
constexpr bool WATCHDOG_TEST_WITHHOLD_FEED = false;

// True once the loop task is registered and the watchdog is active.
bool wdt_active = false;
} // namespace

void beginWatchdog()
{
  // Configure the TWDT timeout and enable the panic handler on timeout so the
  // chip resets (recording a watchdog reset reason) rather than hanging. If
  // the TWDT is already initialized (it is, by the framework), this updates
  // the timeout and panic configuration in place.
  const esp_err_t initErr = esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);

  if (initErr != ESP_OK)
  {
    // The watchdog is a safety net, not a requirement. Log and continue
    // running without it rather than risk a boot loop.
    Serial.printf(
        "Watchdog: init failed (0x%x), continuing without it\n",
        (unsigned)initErr);
    return;
  }

  // Register the current loop task (setup() runs on the loop task). Query the
  // subscription state first so an already-registered task is not treated as
  // an error, which could otherwise cause a boot loop.
  const TaskHandle_t loopTask = xTaskGetCurrentTaskHandle();

  const esp_err_t statusErr = esp_task_wdt_status(loopTask);

  if (statusErr == ESP_OK)
  {
    // Already subscribed (e.g. by the framework); nothing to do.
    wdt_active = true;
    Serial.printf(
        "Watchdog: active (loop task already registered), timeout=%lu s\n",
        (unsigned long)WATCHDOG_TIMEOUT_S);
    return;
  }

  const esp_err_t addErr = esp_task_wdt_add(loopTask);

  if (addErr == ESP_OK || addErr == ESP_ERR_INVALID_ARG)
  {
    // ESP_ERR_INVALID_ARG means the task is already subscribed; treat that as
    // success (defensive, the status check above should have caught it).
    wdt_active = true;
    Serial.printf(
        "Watchdog: active, timeout=%lu s\n",
        (unsigned long)WATCHDOG_TIMEOUT_S);
    return;
  }

  // Any other error: log and continue without the watchdog (no boot loop).
  Serial.printf(
      "Watchdog: add failed (0x%x), continuing without it\n",
      (unsigned)addErr);
}

void feedWatchdog()
{
  if (!wdt_active)
  {
    return;
  }

  if (WATCHDOG_TEST_WITHHOLD_FEED)
  {
    // Test hook: withhold the feed so the watchdog fires after the timeout.
    // Build-time only; must be recompiled to false for production.
    return;
  }

  // Feed on behalf of the current (loop) task. A feed failure is not acted on:
  // if feeding genuinely stops working, the watchdog will fire and reset the
  // chip, which is the desired safety behavior.
  esp_task_wdt_reset();
}
