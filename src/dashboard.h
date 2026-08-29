#pragma once

#include "sensors.h"

void beginDashboard();
void recordDashboardSample(const SensorSnapshot &snapshot);
void drawDashboard(const SensorSnapshot &snapshot);
