# Main-loop watchdog

The firmware arms the ESP32 task watchdog for the Arduino loop task
(`src/watchdog.cpp`). The timeout is `WATCHDOG_TIMEOUT_S` (20 s), chosen to
exceed the worst-case blocking operation in the cooperative loop (a full
e-paper refresh, ~3600 ms) with wide margin, so normal full and partial
refreshes never trip it. The watchdog is fed from `loop()` only; a genuinely
stuck loop stops feeding and the chip resets after the timeout.

After such a reset, the Task 06 reset-reason telemetry
(`air_quality_reset_reason{reason="watchdog"}`) and the boot diagnostic line
identify it as a watchdog reset.

## Hardware validation

1. **Normal operation:** flash the firmware and confirm at least one full
   e-paper refresh cycle (and several partial refreshes) complete without a
   watchdog reset. The boot diagnostic should show the expected reset reason
   (e.g. `power-on` or `software`), not `watchdog`.
2. **Intentional timeout (local physical access only):** set
   `WATCHDOG_TEST_WITHHOLD_FEED` to `true` in `src/watchdog.cpp`, rebuild, and
   flash. The device should reset after ~20 s. After the reset, confirm the
   boot diagnostic and `air_quality_reset_reason` report `watchdog`.
3. **Restore production state:** set `WATCHDOG_TEST_WITHHOLD_FEED` back to
   `false`, rebuild, and flash before considering the task done. The hook is a
   compile-time constant and cannot be triggered remotely or at runtime.
