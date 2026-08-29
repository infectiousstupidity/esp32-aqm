#include "device_info.h"

#include <Preferences.h>
#include <esp_system.h>

#include "version.h"

namespace
{
constexpr char PREFS_NAMESPACE[] = "aqm";
constexpr char PREFS_BOOT_COUNT_KEY[] = "boot_count";

uint32_t boot_count = 0;
uint32_t min_free_heap = 0;
const char *reset_reason_label = "unknown";

const char *labelForResetReason(esp_reset_reason_t reason)
{
  switch (reason)
  {
  case ESP_RST_POWERON:
    return "power-on";
  case ESP_RST_EXT:
    return "external";
  case ESP_RST_SW:
    return "software";
  case ESP_RST_PANIC:
    return "panic";
  case ESP_RST_INT_WDT:
  case ESP_RST_TASK_WDT:
  case ESP_RST_WDT:
    return "watchdog";
  case ESP_RST_DEEPSLEEP:
    return "deep-sleep";
  case ESP_RST_BROWNOUT:
    return "brownout";
  case ESP_RST_SDIO:
    return "sdio";
  case ESP_RST_UNKNOWN:
  default:
    return "unknown";
  }
}
} // namespace

void beginDeviceInfo()
{
  // Read the persisted boot count, increment it once, and write it back.
  // This is the only flash-backed update performed per boot.
  Preferences prefs;
  if (prefs.begin(PREFS_NAMESPACE, false))
  {
    boot_count = (uint32_t)prefs.getInt(PREFS_BOOT_COUNT_KEY, 0) + 1;
    prefs.putInt(PREFS_BOOT_COUNT_KEY, (int32_t)boot_count);
    prefs.end();
  }
  else
  {
    boot_count = 1;
    Serial.println(
        "Device info: Preferences unavailable, boot count not persisted");
  }

  reset_reason_label = labelForResetReason(esp_reset_reason());

  min_free_heap = ESP.getFreeHeap();

  Serial.printf(
      "Boot: version=%s, reset=%s, boot_count=%lu\n",
      FIRMWARE_VERSION,
      reset_reason_label,
      (unsigned long)boot_count);
}

const char *firmwareVersion()
{
  return FIRMWARE_VERSION;
}

const char *resetReasonLabel()
{
  return reset_reason_label;
}

uint32_t bootCount()
{
  return boot_count;
}

void updateMinFreeHeap()
{
  const uint32_t freeHeap = ESP.getFreeHeap();

  if (freeHeap < min_free_heap)
  {
    min_free_heap = freeHeap;
  }
}

uint32_t minFreeHeap()
{
  return min_free_heap;
}
