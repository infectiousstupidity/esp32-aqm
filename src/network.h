#pragma once

#include <stdint.h>

struct NetworkStatus
{
  bool connected;
  int32_t rssiDbm;
};

void beginNetwork();
void serviceNetwork();
NetworkStatus getNetworkStatus();
