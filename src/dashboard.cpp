#include "dashboard.h"

#include <Adafruit_GFX.h>
#include <GxEPD2_BW.h>

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

namespace
{
constexpr uint8_t EPD_CS = 5;
constexpr uint8_t EPD_DC = 19;
constexpr uint8_t EPD_RST = 2;
constexpr uint8_t EPD_BUSY = 4;

constexpr int HISTORY_SIZE = 60;

// Ghosting tuning: force a full refresh after this many partial refreshes.
// Raise to reduce the frequency of the (slower) full refresh; lower to clear
// accumulated ghosting sooner.
constexpr int PARTIALS_BEFORE_FULL_REFRESH = 5;

constexpr int SCREEN_MARGIN = 8;
constexpr int COLUMN_GAP = 12;
constexpr int LABEL_BASELINE_Y = 15;
constexpr int VALUE_BASELINE_Y = 44;
constexpr int UNIT_BASELINE_Y = 59;
constexpr int GRAPH_Y = 68;
constexpr int GRAPH_H = 29;
constexpr int FOOTER_BASELINE_Y = 117;

constexpr float CO2_GRAPH_MIN = 400.0f;
constexpr float CO2_GRAPH_MAX = 2000.0f;
constexpr float PM25_GRAPH_MIN = 0.0f;
constexpr float PM25_GRAPH_MAX = 50.0f;

GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(
    GxEPD2_213_B74(
        EPD_CS,
        EPD_DC,
        EPD_RST,
        EPD_BUSY));

// One slot per scheduled measurement interval. Invalid samples still occupy a
// slot but are marked invalid, so the sparkline renders a gap instead of
// shifting the graph's time axis.
struct HistorySlot
{
  float value = NAN;
  bool valid = false;
};

HistorySlot co2_history[HISTORY_SIZE];
HistorySlot pm25_history[HISTORY_SIZE];
int co2_history_count = 0;
int pm25_history_count = 0;

// Partial refreshes since the last full refresh. Starts at the threshold so
// the first draw after init is a full refresh.
int partials_since_full = PARTIALS_BEFORE_FULL_REFRESH;

void appendHistory(HistorySlot *history, int &count, float value, bool valid)
{
  if (count < HISTORY_SIZE)
  {
    history[count].value = value;
    history[count].valid = valid;
    count++;
    return;
  }

  for (int i = 0; i < HISTORY_SIZE - 1; i++)
  {
    history[i] = history[i + 1];
  }

  history[HISTORY_SIZE - 1].value = value;
  history[HISTORY_SIZE - 1].valid = valid;
}

float clampFloat(float value, float minValue, float maxValue)
{
  if (value < minValue)
    return minValue;

  if (value > maxValue)
    return maxValue;

  return value;
}

int graphYForValue(
    float value,
    float minValue,
    float maxValue,
    int y,
    int h)
{
  value = clampFloat(value, minValue, maxValue);
  const float normalized = (value - minValue) / (maxValue - minValue);

  return y + h - 1 -
         (int)roundf(normalized * (float)(h - 1));
}

void drawSparkline(
    const HistorySlot *history,
    int count,
    int x,
    int y,
    int w,
    int h,
    float minValue,
    float maxValue)
{
  if (count <= 0)
    return;

  const float step = (float)(w - 1) / (float)(HISTORY_SIZE - 1);
  const int historyOffset = HISTORY_SIZE - count;

  bool havePrevious = false;
  int previousX = 0;
  int previousY = 0;
  int lastX = 0;
  int lastY = 0;
  bool haveLast = false;

  for (int i = 0; i < count; i++)
  {
    // Invalid slots are gaps: skip them and break the line so it is not
    // connected across a missing interval.
    if (!history[i].valid)
    {
      havePrevious = false;
      continue;
    }

    const int historyIndex = historyOffset + i;
    const int px = x + (int)roundf(historyIndex * step);
    const int py = graphYForValue(
        history[i].value, minValue, maxValue, y, h);

    if (havePrevious)
    {
      display.drawLine(previousX, previousY, px, py, GxEPD_BLACK);
    }

    previousX = px;
    previousY = py;
    havePrevious = true;

    lastX = px;
    lastY = py;
    haveLast = true;
  }

  if (haveLast)
  {
    display.fillCircle(lastX, lastY, 1, GxEPD_BLACK);
  }
}

void drawPrimaryValue(
    const char *text,
    int x,
    int baselineY,
    int maxWidth)
{
  int16_t boundsX;
  int16_t boundsY;
  uint16_t boundsW;
  uint16_t boundsH;

  display.setFont(&FreeSansBold18pt7b);
  display.getTextBounds(
      text,
      0,
      0,
      &boundsX,
      &boundsY,
      &boundsW,
      &boundsH);

  if ((int)boundsW > maxWidth)
  {
    display.setFont(&FreeSansBold12pt7b);
  }

  display.setCursor(x, baselineY);
  display.print(text);
}

void drawMetricColumn(
    const char *label,
    const char *value,
    const char *unit,
    int x,
    int width)
{
  display.setFont(&FreeSans9pt7b);
  display.setCursor(x, LABEL_BASELINE_Y);
  display.print(label);

  drawPrimaryValue(value, x, VALUE_BASELINE_Y, width);

  display.setFont(&FreeSans9pt7b);
  display.setCursor(x, UNIT_BASELINE_Y);
  display.print(unit);
}

void drawTemperatureFooter(const char *temperature, int x, int baselineY)
{
  display.setFont(&FreeSans9pt7b);
  display.setCursor(x, baselineY);
  display.print(temperature);

  const int textEndX = display.getCursorX();

  // Degree symbol.
  display.drawCircle(textEndX + 3, baselineY - 10, 2, GxEPD_BLACK);
  display.setCursor(textEndX + 8, baselineY);
  display.print("C");
}

void drawRightAlignedText(const char *text, int rightX, int baselineY)
{
  int16_t boundsX;
  int16_t boundsY;
  uint16_t boundsW;
  uint16_t boundsH;

  display.setFont(&FreeSans9pt7b);
  display.getTextBounds(
      text,
      0,
      0,
      &boundsX,
      &boundsY,
      &boundsW,
      &boundsH);
  display.setCursor(rightX - (int)boundsW, baselineY);
  display.print(text);
}
} // namespace

void beginDashboard()
{
  display.init();
  display.setRotation(1);

  // The first draw after init is a full refresh.
  partials_since_full = PARTIALS_BEFORE_FULL_REFRESH;
}

void recordDashboardSample(const SensorSnapshot &snapshot)
{
  // Every scheduled interval occupies one slot, even when invalid, so the
  // graph's time axis is not compressed by missing samples.
  appendHistory(
      co2_history, co2_history_count, snapshot.co2, snapshot.co2Valid);
  appendHistory(
      pm25_history, pm25_history_count, snapshot.pm25, snapshot.pmValid);
}

void drawDashboard(const SensorSnapshot &snapshot)
{
  char co2String[8] = "N/A";
  char pm25String[10] = "N/A";
  char temperatureString[10] = "N/A";
  char humidityString[12] = "N/A";

  if (snapshot.co2Valid)
  {
    snprintf(co2String, sizeof(co2String), "%u", snapshot.co2);
  }

  if (snapshot.pmValid)
  {
    snprintf(pm25String, sizeof(pm25String), "%.1f", snapshot.pm25);
  }

  if (snapshot.temperatureValid)
  {
    snprintf(
        temperatureString,
        sizeof(temperatureString),
        "%.1f",
        snapshot.temperature);
  }

  if (snapshot.humidityValid)
  {
    snprintf(
        humidityString,
        sizeof(humidityString),
        "%.0f%% RH",
        snapshot.humidity);
  }

  const int screenWidth = display.width();
  const int usableWidth = screenWidth - (SCREEN_MARGIN * 2) - COLUMN_GAP;
  const int columnWidth = usableWidth / 2;
  const int leftX = SCREEN_MARGIN;
  const int rightX = SCREEN_MARGIN + columnWidth + COLUMN_GAP;

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

    drawMetricColumn("CO2", co2String, "ppm", leftX, columnWidth);
    drawMetricColumn("PM2.5", pm25String, "ug/m3", rightX, columnWidth);

    drawSparkline(
        co2_history,
        co2_history_count,
        leftX,
        GRAPH_Y,
        columnWidth,
        GRAPH_H,
        CO2_GRAPH_MIN,
        CO2_GRAPH_MAX);

    drawSparkline(
        pm25_history,
        pm25_history_count,
        rightX,
        GRAPH_Y,
        columnWidth,
        GRAPH_H,
        PM25_GRAPH_MIN,
        PM25_GRAPH_MAX);

    drawTemperatureFooter(
        temperatureString,
        SCREEN_MARGIN,
        FOOTER_BASELINE_Y);
    drawRightAlignedText(
        humidityString,
        screenWidth - SCREEN_MARGIN,
        FOOTER_BASELINE_Y);
  } while (display.nextPage());
}
