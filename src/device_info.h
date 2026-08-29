#pragma once

#include <Arduino.h>

// Small device-identity/health state shared between the boot path (main.cpp)
// and the /metrics handler (network.cpp). Deliberately not a general
// diagnostics framework: just the boot-time facts and the min-free-heap
// watermark.
void beginDeviceInfo();

// Firmware version text (single source in version.h).
const char *firmwareVersion();

// Bounded label for the last reset reason (power-on, external, software,
// panic, watchdog, deep-sleep, brownout, sdio, unknown).
const char *resetReasonLabel();

// Persisted boot count, incremented once per boot in beginDeviceInfo().
uint32_t bootCount();

// Refresh the min-free-heap watermark; call once per loop iteration.
void updateMinFreeHeap();

// Minimum free heap observed since boot, in bytes.
uint32_t minFreeHeap();
