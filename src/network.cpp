#include "network.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include <esp_mac.h>

#include "secrets.h"
#include "device_info.h"
#include "sensors.h"

namespace
{
// Hostname prefix; the per-chip suffix makes it unique on a multi-node LAN.
constexpr char HOSTNAME_PREFIX[] = "air-monitor-";

WebServer server(80);
char device_hostname[64];
bool wifi_was_connected = false;
bool wifi_ever_connected = false;
bool mdns_running = false;
uint32_t wifi_reconnects = 0;

// Derive a stable, per-chip hostname from the eFuse base MAC. The last three
// MAC bytes become a six-character hex suffix, so multiple nodes never claim
// the same name and the name survives reboots.
void buildHostname()
{
  uint8_t mac[6];

  if (esp_efuse_mac_get_default(mac) != ESP_OK)
  {
    // No chip identity available; fall back to a non-unique name and say so.
    snprintf(
        device_hostname,
        sizeof device_hostname,
        "%sunknown",
        HOSTNAME_PREFIX);
    Serial.println(
        "mDNS: could not read chip MAC, using non-unique hostname");
    return;
  }

  snprintf(
      device_hostname,
      sizeof device_hostname,
      "%s%02x%02x%02x",
      HOSTNAME_PREFIX,
      mac[3],
      mac[4],
      mac[5]);
}

void startMdns()
{
  if (!MDNS.begin(device_hostname))
  {
    // Discovery is a convenience; a failure must not restart or disable the
    // node. Direct-IP HTTP keeps working either way.
    Serial.println("mDNS: failed to start, continuing without discovery");
    return;
  }

  if (!MDNS.addService("http", "tcp", 80))
  {
    Serial.println("mDNS: failed to advertise http service");
  }

  // Bounded TXT: identify the service and report the firmware version.
  MDNS.addServiceTxt("http", "tcp", "path", "/metrics");
  MDNS.addServiceTxt("http", "tcp", "version", firmwareVersion());

  mdns_running = true;
  Serial.printf("mDNS: advertising %s.local (http, /metrics)\n", device_hostname);
}

void stopMdns()
{
  if (mdns_running)
  {
    MDNS.end();
    mdns_running = false;
  }
}

void startWiFi()
{
  WiFi.mode(WIFI_STA);

  // Keep the radio awake so LAN peers can reliably reach the metrics server.
  WiFi.setSleep(false);
  buildHostname();
  WiFi.setHostname(device_hostname);
  WiFi.setAutoReconnect(true);

  Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void logWiFiStatusChanges()
{
  const bool connected = WiFi.status() == WL_CONNECTED;

  if (connected)
  {
    if (!wifi_was_connected)
    {
      wifi_was_connected = true;

      // A connection after the first is a reconnect.
      if (wifi_ever_connected)
      {
        wifi_reconnects++;
      }
      wifi_ever_connected = true;

      // Advertise over mDNS now that the radio is up; re-begin after any
      // reconnect so the A record reflects the current IP.
      startMdns();

      Serial.println("Wi-Fi connected");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());

      Serial.print("Metrics: http://");
      Serial.print(device_hostname);
      Serial.print(".local/metrics or http://");
      Serial.print(WiFi.localIP());
      Serial.println("/metrics");
    }

    return;
  }

  if (wifi_was_connected)
  {
    wifi_was_connected = false;
    stopMdns();
    Serial.println("Wi-Fi disconnected");
  }
}

void appendSensorMetric(
    String &body,
    const char *metric,
    const char *sensor,
    const String &value)
{
  body += metric;
  body += "{sensor=\"";
  body += sensor;
  body += "\"} ";
  body += value;
  body += "\n";
}

void handleMetrics()
{
  const SensorSnapshot &snapshot = getSensorSnapshot();
  String body;

  // Avoid repeated reallocations.
  body.reserve(3600);

  body +=
      "# HELP air_quality_co2_ppm "
      "Carbon dioxide concentration in parts per million.\n";
  body += "# TYPE air_quality_co2_ppm gauge\n";

  if (snapshot.co2Valid)
  {
    body += "air_quality_co2_ppm ";
    body += String(snapshot.co2);
    body += "\n";
  }

  body +=
      "# HELP air_quality_pm10_ug_m3 "
      "PM1.0 mass concentration in micrograms per cubic meter.\n";
  body += "# TYPE air_quality_pm10_ug_m3 gauge\n";

  if (snapshot.pmValid)
  {
    body += "air_quality_pm10_ug_m3 ";
    body += String(snapshot.pm10, 2);
    body += "\n";
  }

  body +=
      "# HELP air_quality_pm25_ug_m3 "
      "PM2.5 mass concentration in micrograms per cubic meter.\n";
  body += "# TYPE air_quality_pm25_ug_m3 gauge\n";

  if (snapshot.pmValid)
  {
    body += "air_quality_pm25_ug_m3 ";
    body += String(snapshot.pm25, 2);
    body += "\n";
  }

  body +=
      "# HELP air_quality_pm40_ug_m3 "
      "PM4.0 mass concentration in micrograms per cubic meter.\n";
  body += "# TYPE air_quality_pm40_ug_m3 gauge\n";

  if (snapshot.pmValid)
  {
    body += "air_quality_pm40_ug_m3 ";
    body += String(snapshot.pm40, 2);
    body += "\n";
  }

  body +=
      "# HELP air_quality_pm100_ug_m3 "
      "PM10.0 mass concentration in micrograms per cubic meter.\n";
  body += "# TYPE air_quality_pm100_ug_m3 gauge\n";

  if (snapshot.pmValid)
  {
    body += "air_quality_pm100_ug_m3 ";
    body += String(snapshot.pm100, 2);
    body += "\n";
  }

  body +=
      "# HELP air_quality_typical_particle_size_um "
      "Typical particle size in micrometers.\n";
  body += "# TYPE air_quality_typical_particle_size_um gauge\n";

  if (snapshot.pmValid)
  {
    body += "air_quality_typical_particle_size_um ";
    body += String(snapshot.typicalParticleSize, 2);
    body += "\n";
  }

  body +=
      "# HELP air_quality_temperature_celsius "
      "Ambient temperature in degrees Celsius.\n";
  body += "# TYPE air_quality_temperature_celsius gauge\n";

  if (snapshot.temperatureValid)
  {
    body += "air_quality_temperature_celsius ";
    body += String(snapshot.temperature, 2);
    body += "\n";
  }

  body +=
      "# HELP air_quality_humidity_percent "
      "Relative humidity percentage.\n";
  body += "# TYPE air_quality_humidity_percent gauge\n";

  if (snapshot.humidityValid)
  {
    body += "air_quality_humidity_percent ";
    body += String(snapshot.humidity, 2);
    body += "\n";
  }

  body +=
      "# HELP air_quality_pressure_pa "
      "Ambient pressure in pascals.\n";
  body += "# TYPE air_quality_pressure_pa gauge\n";

  if (snapshot.pressureValid)
  {
    body += "air_quality_pressure_pa ";
    body += String(snapshot.pressure, 1);
    body += "\n";
  }

  body +=
      "# HELP air_quality_sensor_up "
      "Whether the sensor is initialized and running.\n";
  body += "# TYPE air_quality_sensor_up gauge\n";

  appendSensorMetric(
      body, "air_quality_sensor_up", "bme280",
      snapshot.bme280.running ? "1" : "0");
  appendSensorMetric(
      body, "air_quality_sensor_up", "scd41",
      snapshot.scd41.running ? "1" : "0");
  appendSensorMetric(
      body, "air_quality_sensor_up", "sps30",
      snapshot.sps30.running ? "1" : "0");

  body +=
      "# HELP air_quality_measurement_valid "
      "Whether the latest measurement cycle produced a valid value.\n";
  body += "# TYPE air_quality_measurement_valid gauge\n";

  body += "air_quality_measurement_valid{measurement=\"temperature\"} ";
  body += snapshot.temperatureValid ? "1\n" : "0\n";
  body += "air_quality_measurement_valid{measurement=\"humidity\"} ";
  body += snapshot.humidityValid ? "1\n" : "0\n";
  body += "air_quality_measurement_valid{measurement=\"pressure\"} ";
  body += snapshot.pressureValid ? "1\n" : "0\n";
  body += "air_quality_measurement_valid{measurement=\"co2\"} ";
  body += snapshot.co2Valid ? "1\n" : "0\n";
  body += "air_quality_measurement_valid{measurement=\"pm25\"} ";
  body += snapshot.pmValid ? "1\n" : "0\n";

  body +=
      "# HELP air_quality_sensor_read_errors_total "
      "Cumulative sensor read errors since boot.\n";
  body += "# TYPE air_quality_sensor_read_errors_total counter\n";

  appendSensorMetric(
      body, "air_quality_sensor_read_errors_total", "bme280",
      String(snapshot.bme280.readErrors));
  appendSensorMetric(
      body, "air_quality_sensor_read_errors_total", "scd41",
      String(snapshot.scd41.readErrors));
  appendSensorMetric(
      body, "air_quality_sensor_read_errors_total", "sps30",
      String(snapshot.sps30.readErrors));

  body +=
      "# HELP air_quality_sensor_last_success_age_seconds "
      "Seconds since the sensor's last successful sample. "
      "Absent until the sensor has succeeded once.\n";
  body += "# TYPE air_quality_sensor_last_success_age_seconds gauge\n";

  if (snapshot.bme280.hasSample)
  {
    appendSensorMetric(
        body,
        "air_quality_sensor_last_success_age_seconds",
        "bme280",
        String((millis() - snapshot.bme280.lastSuccessMs) / 1000UL));
  }

  if (snapshot.scd41.hasSample)
  {
    appendSensorMetric(
        body,
        "air_quality_sensor_last_success_age_seconds",
        "scd41",
        String((millis() - snapshot.scd41.lastSuccessMs) / 1000UL));
  }

  if (snapshot.sps30.hasSample)
  {
    appendSensorMetric(
        body,
        "air_quality_sensor_last_success_age_seconds",
        "sps30",
        String((millis() - snapshot.sps30.lastSuccessMs) / 1000UL));
  }

  body +=
      "# HELP air_quality_uptime_seconds "
      "ESP32 uptime in seconds.\n";
  body += "# TYPE air_quality_uptime_seconds gauge\n";
  body += "air_quality_uptime_seconds ";
  body += String(millis() / 1000UL);
  body += "\n";

  body +=
      "# HELP air_quality_firmware_info "
      "Firmware build information; value is always 1.\n";
  body += "# TYPE air_quality_firmware_info gauge\n";
  body += "air_quality_firmware_info{version=\"";
  body += firmwareVersion();
  body += "\"} 1\n";

  body +=
      "# HELP air_quality_reset_reason "
      "Reason for the last reset; value is always 1.\n";
  body += "# TYPE air_quality_reset_reason gauge\n";
  body += "air_quality_reset_reason{reason=\"";
  body += resetReasonLabel();
  body += "\"} 1\n";

  body +=
      "# HELP air_quality_boot_count "
      "Number of boots, persisted across resets.\n";
  body += "# TYPE air_quality_boot_count gauge\n";
  body += "air_quality_boot_count ";
  body += String(bootCount());
  body += "\n";

  body +=
      "# HELP air_quality_free_heap_bytes "
      "Current free heap in bytes.\n";
  body += "# TYPE air_quality_free_heap_bytes gauge\n";
  body += "air_quality_free_heap_bytes ";
  body += String(ESP.getFreeHeap());
  body += "\n";

  body +=
      "# HELP air_quality_min_free_heap_bytes "
      "Minimum free heap in bytes since boot.\n";
  body += "# TYPE air_quality_min_free_heap_bytes gauge\n";
  body += "air_quality_min_free_heap_bytes ";
  body += String(minFreeHeap());
  body += "\n";

  body +=
      "# HELP air_quality_wifi_reconnects_total "
      "Wi-Fi reconnects since boot.\n";
  body += "# TYPE air_quality_wifi_reconnects_total counter\n";
  body += "air_quality_wifi_reconnects_total ";
  body += String(wifi_reconnects);
  body += "\n";

  body +=
      "# HELP air_quality_wifi_rssi_dbm "
      "Wi-Fi signal strength in dBm.\n";
  body += "# TYPE air_quality_wifi_rssi_dbm gauge\n";

  if (WiFi.status() == WL_CONNECTED)
  {
    body += "air_quality_wifi_rssi_dbm ";
    body += String(WiFi.RSSI());
    body += "\n";
  }

  server.sendHeader("Cache-Control", "no-cache");
  server.send(
      200,
      "text/plain; version=0.0.4; charset=utf-8",
      body);
}

void handleRoot()
{
  String body;

  body += "Air quality monitor\n";
  body += "\n";
  body += "Prometheus metrics:\n";
  body += "/metrics\n";
  body += "\n";
  body += "mDNS hostname: ";
  body += device_hostname;
  body += ".local\n";

  server.send(200, "text/plain; charset=utf-8", body);
}

void startHttpServer()
{
  server.on("/", HTTP_GET, handleRoot);
  server.on("/metrics", HTTP_GET, handleMetrics);
  server.onNotFound(
      []()
      {
        server.send(404, "text/plain", "Not found\n");
      });

  server.begin();
  Serial.println("HTTP server started");
}
} // namespace

void beginNetwork()
{
  startWiFi();
  startHttpServer();
}

void serviceNetwork()
{
  // Requests are serviced cooperatively between measurement/display updates.
  server.handleClient();
  logWiFiStatusChanges();
}
