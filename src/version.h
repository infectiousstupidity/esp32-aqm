#pragma once

// Single source of the firmware version text. Update this one constant for a
// release; boot diagnostics and /metrics read it without further edits.
constexpr const char *FIRMWARE_VERSION = "0.6.0";
