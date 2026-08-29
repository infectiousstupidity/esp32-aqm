#include "dashboard.h"

#include <Adafruit_GFX.h>
#include <GxEPD2_BW.h>

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

#include "network.h"

namespace
{
constexpr uint8_t EPD_CS = 5;
constexpr uint8_t EPD_DC = 19;
constexpr uint8_t EPD_RST = 2;
constexpr uint8_t EPD_BUSY = 4;

// Ghosting tuning: force a full refresh after this many partial refreshes.
// Raise to reduce the frequency of the (slower) full refresh; lower to clear
// accumulated ghosting sooner.
constexpr int PARTIALS_BEFORE_FULL_REFRESH = 2;

constexpr float PI_RADIANS = 3.14159265f;
constexpr int SCORE_RING_CENTER_X = 51;
constexpr int SCORE_RING_CENTER_Y = 50;
constexpr int SCORE_RING_OUTER_RADIUS = 42;
constexpr int SCORE_RING_INNER_RADIUS = 35;
constexpr int SCORE_RING_DOT_RADIUS = 39;
constexpr int SCORE_RING_SEGMENTS = 60;
constexpr float SCORE_RING_START_RADIANS = -PI_RADIANS / 2.0f;
constexpr float SCORE_RING_STEP_RADIANS =
    (2.0f * PI_RADIANS) / SCORE_RING_SEGMENTS;

constexpr int SCORE_BASELINE_Y = 61;
constexpr int STATUS_X = 96;
constexpr int STATUS_BASELINE_Y = 20;
constexpr int WIFI_ICON_CENTER_X = 233;
constexpr int WIFI_ICON_DOT_Y = 22;
constexpr int WIFI_ICON_LEFT_X = 224;
constexpr int WIFI_ICON_RIGHT_X = 242;
constexpr int WIFI_ICON_TOP_Y = 5;
constexpr int WIFI_ICON_BOTTOM_Y = 23;
constexpr int SUBTITLE_BASELINE_Y = 35;
constexpr int SENSOR_ERROR_SUBTITLE_TOP_Y = 28;

constexpr int WARNING_ICON_LEFT_X = 205;
constexpr int WARNING_ICON_CENTER_X = 213;
constexpr int WARNING_ICON_RIGHT_X = 221;
constexpr int WARNING_ICON_TOP_Y = 6;
constexpr int WARNING_ICON_BOTTOM_Y = 23;

constexpr int32_t WIFI_STRONG_MIN_RSSI_DBM = -67;
constexpr int32_t WIFI_MEDIUM_MIN_RSSI_DBM = -75;
constexpr uint32_t PERSISTENT_SENSOR_ERROR_COUNT = 3;
constexpr uint32_t SCORE_DATA_STALE_AFTER_MS = 90000;

constexpr int LEFT_METRIC_X = 108;
constexpr int LEFT_METRIC_WIDTH = 58;
constexpr int RIGHT_METRIC_X = 178;
constexpr int RIGHT_METRIC_WIDTH = 64;
constexpr int METRIC_LABEL_BASELINE_Y = 50;
constexpr int METRIC_VALUE_BASELINE_Y = 74;
constexpr int METRIC_UNIT_TOP_Y = 81;

constexpr int FOOTER_LINE_Y = 97;
constexpr int FOOTER_BASELINE_Y = 119;
constexpr int THERMOMETER_X = 14;
constexpr int THERMOMETER_TOP_Y = 104;
constexpr int TEMPERATURE_X = 25;
constexpr int DROPLET_CENTER_X = 163;
constexpr int DROPLET_TOP_Y = 103;
constexpr int HUMIDITY_RIGHT_X = 242;

struct ScoreAnchor
{
  float value;
  uint8_t score;
};

enum class WifiSignal
{
  Offline,
  Weak,
  Medium,
  Strong,
};

enum class DashboardHealth
{
  Healthy,
  NetworkOffline,
  DataStale,
  SensorError,
};

struct DashboardUiState
{
  DashboardHealth health;
  WifiSignal wifiSignal;
  bool co2Error;
  bool pmError;
  bool environmentError;
  bool scoreDataStale;
};

constexpr ScoreAnchor CO2_SCORE_ANCHORS[] = {
    {600.0f, 100},
    {800.0f, 90},
    {1000.0f, 75},
    {1400.0f, 50},
    {2000.0f, 25},
    {3000.0f, 0},
};

constexpr ScoreAnchor PM25_SCORE_ANCHORS[] = {
    {5.0f, 100},
    {10.0f, 90},
    {15.0f, 75},
    {25.0f, 50},
    {50.0f, 25},
    {100.0f, 0},
};

GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(
    GxEPD2_213_B74(
        EPD_CS,
        EPD_DC,
        EPD_RST,
        EPD_BUSY));

// Partial refreshes since the last full refresh. Starts at the threshold so
// the first draw after init is a full refresh.
int partials_since_full = PARTIALS_BEFORE_FULL_REFRESH;

uint8_t interpolateScore(
    float value,
    const ScoreAnchor *anchors,
    size_t anchorCount)
{
  if (isnan(value))
    return 0;

  if (value <= anchors[0].value)
    return anchors[0].score;

  for (size_t i = 1; i < anchorCount; i++)
  {
    if (value <= anchors[i].value)
    {
      const ScoreAnchor &low = anchors[i - 1];
      const ScoreAnchor &high = anchors[i];
      const float position =
          (value - low.value) / (high.value - low.value);
      const float score =
          low.score + position * (high.score - low.score);

      return (uint8_t)roundf(score);
    }
  }

  return anchors[anchorCount - 1].score;
}

uint8_t indoorAirScore(float co2, float pm25)
{
  const uint8_t co2Score = interpolateScore(
      co2,
      CO2_SCORE_ANCHORS,
      sizeof(CO2_SCORE_ANCHORS) / sizeof(CO2_SCORE_ANCHORS[0]));
  const uint8_t pm25Score = interpolateScore(
      pm25,
      PM25_SCORE_ANCHORS,
      sizeof(PM25_SCORE_ANCHORS) / sizeof(PM25_SCORE_ANCHORS[0]));

  return co2Score < pm25Score ? co2Score : pm25Score;
}

const char *scoreLabel(uint8_t score)
{
  if (score >= 90)
    return "EXCELLENT";

  if (score >= 75)
    return "GOOD";

  if (score >= 50)
    return "MODERATE";

  if (score >= 25)
    return "POOR";

  return "BAD";
}

WifiSignal wifiSignalForStatus(const NetworkStatus &status)
{
  if (!status.connected)
    return WifiSignal::Offline;

  if (status.rssiDbm >= WIFI_STRONG_MIN_RSSI_DBM)
    return WifiSignal::Strong;

  if (status.rssiDbm >= WIFI_MEDIUM_MIN_RSSI_DBM)
    return WifiSignal::Medium;

  return WifiSignal::Weak;
}

bool hasPersistentError(const SensorHealth &health)
{
  return !health.running ||
         health.consecutiveErrors >= PERSISTENT_SENSOR_ERROR_COUNT;
}

bool hasStaleScoreData(
    const SensorHealth &health,
    bool persistentError,
    uint32_t now)
{
  return health.hasSample &&
         !persistentError &&
         now - health.lastSuccessMs > SCORE_DATA_STALE_AFTER_MS;
}

DashboardUiState resolveDashboardState(
    const SensorSnapshot &snapshot,
    const NetworkStatus &networkStatus,
    uint32_t now)
{
  DashboardUiState state;
  state.wifiSignal = wifiSignalForStatus(networkStatus);
  state.co2Error = hasPersistentError(snapshot.scd41);
  state.pmError = hasPersistentError(snapshot.sps30);
  state.environmentError = hasPersistentError(snapshot.bme280);
  state.scoreDataStale =
      hasStaleScoreData(snapshot.scd41, state.co2Error, now) ||
      hasStaleScoreData(snapshot.sps30, state.pmError, now);

  if (state.co2Error || state.pmError || state.environmentError)
  {
    state.health = DashboardHealth::SensorError;
  }
  else if (state.scoreDataStale)
  {
    state.health = DashboardHealth::DataStale;
  }
  else if (state.wifiSignal == WifiSignal::Offline)
  {
    state.health = DashboardHealth::NetworkOffline;
  }
  else
  {
    state.health = DashboardHealth::Healthy;
  }

  return state;
}

const char *sensorErrorSubtitle(
    bool co2Error,
    bool pmError,
    bool environmentError)
{
  const int errorCount =
      (co2Error ? 1 : 0) +
      (pmError ? 1 : 0) +
      (environmentError ? 1 : 0);

  if (errorCount > 1)
    return "SENSOR ERROR";

  if (co2Error)
    return "CO2 SENSOR ERROR";

  if (pmError)
    return "PM SENSOR ERROR";

  if (environmentError)
    return "ENV SENSOR ERROR";

  return nullptr;
}

const char *dashboardSubtitle(const DashboardUiState &state)
{
  switch (state.health)
  {
  case DashboardHealth::SensorError:
    return sensorErrorSubtitle(
        state.co2Error,
        state.pmError,
        state.environmentError);
  case DashboardHealth::DataStale:
    return "DATA STALE";
  case DashboardHealth::NetworkOffline:
    return "OFFLINE";
  case DashboardHealth::Healthy:
  default:
    return nullptr;
  }
}

void drawCenteredText(
    const char *text,
    int centerX,
    int baselineY,
    const GFXfont *font)
{
  int16_t boundsX;
  int16_t boundsY;
  uint16_t boundsW;
  uint16_t boundsH;

  display.setFont(font);
  display.getTextBounds(
      text,
      0,
      0,
      &boundsX,
      &boundsY,
      &boundsW,
      &boundsH);
  display.setCursor(
      centerX - boundsX - (int)boundsW / 2,
      baselineY);
  display.print(text);
}

void drawScoreRing(uint8_t score)
{
  const int filledSegments =
      (score * SCORE_RING_SEGMENTS + 50) / 100;

  for (int i = 0; i < SCORE_RING_SEGMENTS; i++)
  {
    const float angle =
        SCORE_RING_START_RADIANS + i * SCORE_RING_STEP_RADIANS;
    const float cosine = cosf(angle);
    const float sine = sinf(angle);

    if (i < filledSegments)
    {
      const int innerX =
          SCORE_RING_CENTER_X + (int)roundf(cosine * SCORE_RING_INNER_RADIUS);
      const int innerY =
          SCORE_RING_CENTER_Y + (int)roundf(sine * SCORE_RING_INNER_RADIUS);
      const int outerX =
          SCORE_RING_CENTER_X + (int)roundf(cosine * SCORE_RING_OUTER_RADIUS);
      const int outerY =
          SCORE_RING_CENTER_Y + (int)roundf(sine * SCORE_RING_OUTER_RADIUS);
      display.drawLine(innerX, innerY, outerX, outerY, GxEPD_BLACK);
      display.fillCircle(
          (innerX + outerX) / 2,
          (innerY + outerY) / 2,
          1,
          GxEPD_BLACK);
    }
    else
    {
      const int dotX =
          SCORE_RING_CENTER_X + (int)roundf(cosine * SCORE_RING_DOT_RADIUS);
      const int dotY =
          SCORE_RING_CENTER_Y + (int)roundf(sine * SCORE_RING_DOT_RADIUS);
      display.drawPixel(dotX, dotY, GxEPD_BLACK);
    }
  }
}

void drawWifiArcs(int arcCount)
{
  if (arcCount >= 3)
  {
    display.drawLine(224, 12, 226, 9, GxEPD_BLACK);
    display.drawLine(226, 9, 229, 7, GxEPD_BLACK);
    display.drawLine(229, 7, WIFI_ICON_CENTER_X, 6, GxEPD_BLACK);
    display.drawLine(WIFI_ICON_CENTER_X, 6, 237, 7, GxEPD_BLACK);
    display.drawLine(237, 7, 240, 9, GxEPD_BLACK);
    display.drawLine(240, 9, 242, 12, GxEPD_BLACK);
  }

  if (arcCount >= 2)
  {
    display.drawLine(227, 15, 229, 13, GxEPD_BLACK);
    display.drawLine(229, 13, WIFI_ICON_CENTER_X, 11, GxEPD_BLACK);
    display.drawLine(WIFI_ICON_CENTER_X, 11, 237, 13, GxEPD_BLACK);
    display.drawLine(237, 13, 239, 15, GxEPD_BLACK);
  }

  if (arcCount >= 1)
  {
    display.drawLine(230, 18, WIFI_ICON_CENTER_X, 16, GxEPD_BLACK);
    display.drawLine(WIFI_ICON_CENTER_X, 16, 236, 18, GxEPD_BLACK);
  }

  display.fillCircle(
      WIFI_ICON_CENTER_X,
      WIFI_ICON_DOT_Y,
      1,
      GxEPD_BLACK);
}

void drawWifiIcon(WifiSignal signal)
{
  if (signal == WifiSignal::Offline)
  {
    drawWifiArcs(3);

    // Clear a narrow channel through the signal, then add a crisp slash.
    display.drawLine(
        WIFI_ICON_LEFT_X - 1,
        WIFI_ICON_TOP_Y,
        WIFI_ICON_RIGHT_X - 1,
        WIFI_ICON_BOTTOM_Y,
        GxEPD_WHITE);
    display.drawLine(
        WIFI_ICON_LEFT_X + 1,
        WIFI_ICON_TOP_Y,
        WIFI_ICON_RIGHT_X + 1,
        WIFI_ICON_BOTTOM_Y,
        GxEPD_WHITE);
    display.drawLine(
        WIFI_ICON_LEFT_X,
        WIFI_ICON_TOP_Y,
        WIFI_ICON_RIGHT_X,
        WIFI_ICON_BOTTOM_Y,
        GxEPD_BLACK);
    return;
  }

  if (signal == WifiSignal::Strong)
  {
    drawWifiArcs(3);
  }
  else if (signal == WifiSignal::Medium)
  {
    drawWifiArcs(2);
  }
  else
  {
    drawWifiArcs(1);
  }
}

void drawWarningIcon()
{
  display.drawTriangle(
      WARNING_ICON_CENTER_X,
      WARNING_ICON_TOP_Y,
      WARNING_ICON_LEFT_X,
      WARNING_ICON_BOTTOM_Y,
      WARNING_ICON_RIGHT_X,
      WARNING_ICON_BOTTOM_Y,
      GxEPD_BLACK);
  display.drawLine(
      WARNING_ICON_CENTER_X,
      WARNING_ICON_TOP_Y + 5,
      WARNING_ICON_CENTER_X,
      WARNING_ICON_BOTTOM_Y - 5,
      GxEPD_BLACK);
  display.fillCircle(
      WARNING_ICON_CENTER_X,
      WARNING_ICON_BOTTOM_Y - 2,
      1,
      GxEPD_BLACK);
}

void drawStatus(const char *label)
{
  display.setFont(&FreeSansBold9pt7b);
  display.setCursor(STATUS_X, STATUS_BASELINE_Y);
  display.print(label);
}

void drawSubtitle(const char *subtitle, bool compact)
{
  if (subtitle == nullptr)
    return;

  if (compact)
  {
    // The exact sensor error labels need the compact built-in 5x7 font to
    // fit the 145-pixel subtitle area without abbreviation.
    display.setFont(nullptr);
    display.setTextSize(1);
    display.setCursor(STATUS_X, SENSOR_ERROR_SUBTITLE_TOP_Y);
  }
  else
  {
    display.setFont(&FreeSans9pt7b);
    display.setCursor(STATUS_X, SUBTITLE_BASELINE_Y);
  }

  display.print(subtitle);
}

void drawMetric(
    const char *label,
    const char *value,
    const char *unit,
    int x,
    int width)
{
  drawCenteredText(
      label,
      x + width / 2,
      METRIC_LABEL_BASELINE_Y,
      &FreeSans9pt7b);

  int16_t boundsX;
  int16_t boundsY;
  uint16_t boundsW;
  uint16_t boundsH;
  display.setFont(&FreeSansBold12pt7b);
  display.getTextBounds(
      value,
      0,
      0,
      &boundsX,
      &boundsY,
      &boundsW,
      &boundsH);

  const int valueMaxWidth = width;
  const GFXfont *valueFont = &FreeSansBold12pt7b;
  if ((int)boundsW > valueMaxWidth)
  {
    valueFont = &FreeSansBold9pt7b;
  }

  drawCenteredText(
      value,
      x + width / 2,
      METRIC_VALUE_BASELINE_Y,
      valueFont);

  display.setFont(nullptr);
  display.setTextSize(1);
  display.setCursor(
      x + (width - (int)strlen(unit) * 6) / 2,
      METRIC_UNIT_TOP_Y);
  display.print(unit);
}

void drawThermometerIcon()
{
  display.drawRoundRect(
      THERMOMETER_X,
      THERMOMETER_TOP_Y,
      5,
      13,
      2,
      GxEPD_BLACK);
  display.drawLine(
      THERMOMETER_X + 2,
      THERMOMETER_TOP_Y + 5,
      THERMOMETER_X + 2,
      THERMOMETER_TOP_Y + 12,
      GxEPD_BLACK);
  display.fillCircle(
      THERMOMETER_X + 2,
      THERMOMETER_TOP_Y + 13,
      3,
      GxEPD_BLACK);
}

void drawDropletIcon()
{
  display.fillTriangle(
      DROPLET_CENTER_X,
      DROPLET_TOP_Y,
      DROPLET_CENTER_X - 5,
      DROPLET_TOP_Y + 10,
      DROPLET_CENTER_X + 5,
      DROPLET_TOP_Y + 10,
      GxEPD_BLACK);
  display.fillCircle(
      DROPLET_CENTER_X,
      DROPLET_TOP_Y + 10,
      5,
      GxEPD_BLACK);
}

void drawTemperature(const char *temperature, bool showUnit)
{
  display.setFont(&FreeSansBold9pt7b);
  display.setCursor(TEMPERATURE_X, FOOTER_BASELINE_Y);
  display.print(temperature);

  if (!showUnit)
    return;

  const int textEndX = display.getCursorX();
  display.drawCircle(textEndX + 3, FOOTER_BASELINE_Y - 11, 2, GxEPD_BLACK);
  display.setCursor(textEndX + 8, FOOTER_BASELINE_Y);
  display.print("C");
}

void drawHumidity(const char *humidity)
{
  int16_t boundsX;
  int16_t boundsY;
  uint16_t boundsW;
  uint16_t boundsH;

  display.setFont(&FreeSansBold9pt7b);
  display.getTextBounds(
      humidity,
      0,
      0,
      &boundsX,
      &boundsY,
      &boundsW,
      &boundsH);
  display.setCursor(
      HUMIDITY_RIGHT_X - boundsX - (int)boundsW,
      FOOTER_BASELINE_Y);
  display.print(humidity);
}
} // namespace

void beginDashboard()
{
  display.init();
  display.setRotation(1);

  // The first draw after init is a full refresh.
  partials_since_full = PARTIALS_BEFORE_FULL_REFRESH;
}

void drawDashboard(const SensorSnapshot &snapshot)
{
  const NetworkStatus networkStatus = getNetworkStatus();
  const DashboardUiState state = resolveDashboardState(
      snapshot,
      networkStatus,
      millis());
  const bool hasWarning =
      state.health == DashboardHealth::SensorError ||
      state.health == DashboardHealth::DataStale;
  const bool scoreAvailable = !state.co2Error && !state.pmError;

  char co2String[8] = "N/A";
  char pm25String[10] = "N/A";
  char temperatureString[10] = "N/A";
  char humidityString[8] = "N/A";
  const char *co2Unit = "ppm";
  const char *pm25Unit = "ug/m3";

  if (state.co2Error)
  {
    strcpy(co2String, "--");
    co2Unit = "ERROR";
  }
  else if (snapshot.co2Valid ||
           (state.scoreDataStale && snapshot.scd41.hasSample))
  {
    snprintf(co2String, sizeof(co2String), "%u", snapshot.co2);
  }

  if (state.pmError)
  {
    strcpy(pm25String, "--");
    pm25Unit = "ERROR";
  }
  else if (snapshot.pmValid ||
           (state.scoreDataStale && snapshot.sps30.hasSample))
  {
    snprintf(pm25String, sizeof(pm25String), "%.1f", snapshot.pm25);
  }

  if (state.environmentError)
  {
    strcpy(temperatureString, "--");
    strcpy(humidityString, "--");
  }
  else if (snapshot.temperatureValid)
  {
    snprintf(
        temperatureString,
        sizeof(temperatureString),
        "%.1f",
        snapshot.temperature);
  }

  if (!state.environmentError && snapshot.humidityValid)
  {
    snprintf(
        humidityString,
        sizeof(humidityString),
        "%.0f%%",
        snapshot.humidity);
  }

  const uint8_t score = scoreAvailable
                            ? indoorAirScore(snapshot.co2, snapshot.pm25)
                            : 0;
  char scoreString[4];
  if (scoreAvailable)
  {
    snprintf(scoreString, sizeof(scoreString), "%u", score);
  }
  else
  {
    strcpy(scoreString, "--");
  }

  // Full refresh on the first draw and after PARTIALS_BEFORE_FULL_REFRESH
  // partial refreshes (ghosting tuning); partial refresh otherwise.
  if (partials_since_full >= PARTIALS_BEFORE_FULL_REFRESH)
  {
    display.setFullWindow();
    partials_since_full = 0;
  }
  else
  {
    display.setPartialWindow(0, 0, display.width(), display.height());
    partials_since_full++;
  }

  display.firstPage();

  do
  {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    drawScoreRing(score);
    drawCenteredText(
        scoreString,
        SCORE_RING_CENTER_X,
        SCORE_BASELINE_Y,
        &FreeSansBold18pt7b);

    drawStatus(scoreAvailable ? scoreLabel(score) : "DEGRADED");
    if (hasWarning)
    {
      drawWarningIcon();
    }
    drawWifiIcon(state.wifiSignal);
    drawSubtitle(
        dashboardSubtitle(state),
        state.health == DashboardHealth::SensorError);

    drawMetric(
        "CO2",
        co2String,
        co2Unit,
        LEFT_METRIC_X,
        LEFT_METRIC_WIDTH);
    drawMetric(
        "PM2.5",
        pm25String,
        pm25Unit,
        RIGHT_METRIC_X,
        RIGHT_METRIC_WIDTH);

    display.drawLine(7, FOOTER_LINE_Y, 243, FOOTER_LINE_Y, GxEPD_BLACK);
    drawThermometerIcon();
    drawTemperature(temperatureString, !state.environmentError);
    drawDropletIcon();
    drawHumidity(humidityString);
  } while (display.nextPage());
}
