#pragma once

// ESP32 task-watchdog for the Arduino loop task.
//
// beginWatchdog() configures the task watchdog timeout and registers the loop
// task once. feedWatchdog() is called from the cooperative loop() to keep the
// watchdog from firing. A genuinely stuck loop stops feeding and the chip
// resets after the timeout; the reset is then reported by the Task 06
// reset-reason telemetry as "watchdog".
void beginWatchdog();
void feedWatchdog();
