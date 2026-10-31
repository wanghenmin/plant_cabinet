#include <Arduino.h>
#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_SHT31.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <math.h>

const char *WIFI_SSID = "Redmi K70";
const char *WIFI_PASSWORD = "qwerzxcv234er";
const char *BACKEND_URL = "http://10.15.179.184:8000";
const char *DEVICE_ID = "plant-box-01";
const char *DEVICE_TOKEN = "replace-with-a-long-random-token";

constexpr uint8_t SOIL_PIN = 4;
constexpr uint8_t PUMP_PIN = 18;
constexpr uint8_t LIGHT_PIN = 15;
constexpr uint8_t I2C_SDA_PIN = 8;
constexpr uint8_t I2C_SCL_PIN = 9;

constexpr int SOIL_DRY_RAW = 2800;
constexpr int SOIL_WET_RAW = 1300;
constexpr uint32_t SENSOR_INTERVAL_MS = 2000;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 10000;
constexpr uint32_t DEVICE_POLL_INTERVAL_MS = 3000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
constexpr uint32_t HARD_MAX_PUMP_RUN_MS = 3000;
constexpr uint32_t HARD_MAX_DAILY_PUMP_MS = 60000;

constexpr uint16_t PWM_FREQUENCY = 5000;
constexpr uint8_t PWM_RESOLUTION = 12;
constexpr uint16_t PWM_MAX = 4095;
int soilStartThreshold = 30;
uint32_t waterPulseMs = 1500;
uint32_t waterIntervalMs = 600000;
uint32_t maxDailyPumpMs = 60000;
int lightOnMinute = 480;
int lightOffMinute = 1200;
uint16_t lightDuty = PWM_MAX;
bool scheduleEnabled = true;
bool automaticLightEnabled = false;
int lightLuxThreshold = 300;
int lightLuxHysteresis = 100;
bool automaticLightOn = false;

BH1750 lightMeter;
Adafruit_SHT31 sht31;

bool lightSensorReady = false;
bool sht31Ready = false;
bool pumpRunning = false;
bool automaticWateringEnabled = false;
bool manualLightMode = false;
bool manualLightOn = false;
bool hasWatered = false;

uint32_t pumpStartedAt = 0;
uint32_t pumpStopAt = 0;
uint32_t lastWaterEndedAt = 0;
uint32_t dailyWindowStartedAt = 0;
uint32_t dailyPumpRuntimeMs = 0;
uint32_t lastAutoWaterAttemptAt = 0;
uint32_t lastTelemetryAt = 0;
uint32_t lastDevicePollAt = 0;
uint32_t lastWifiAttemptAt = 0;
uint32_t lastSensorAt = 0;
int pendingCommandResultId = 0;
bool pendingCommandResultSuccess = false;
String pendingCommandResultMessage;
int latestSoilRaw = -1;
int latestSoilPercent = -1;
float latestLux = NAN;
float latestTemperature = NAN;
float latestHumidity = NAN;

void resetDailyBudgetIfNeeded() {
  if (dailyWindowStartedAt == 0 || millis() - dailyWindowStartedAt >= 86400000UL) {
    dailyWindowStartedAt = millis();
    dailyPumpRuntimeMs = 0;
  }
}

int readSoilRaw() {
  uint32_t total = 0;
  for (uint8_t sample = 0; sample < 8; ++sample) {
    total += analogRead(SOIL_PIN);
    delayMicroseconds(500);
  }
  return total / 8;
}

int soilPercentFromRaw(int raw) {
  long percent = map(raw, SOIL_DRY_RAW, SOIL_WET_RAW, 0, 100);
  return constrain(static_cast<int>(percent), 0, 100);
}

void stopPump(const char *reason) {
  digitalWrite(PUMP_PIN, LOW);
  if (!pumpRunning) return;
  dailyPumpRuntimeMs += millis() - pumpStartedAt;
  pumpRunning = false;
  lastWaterEndedAt = millis();
  Serial.print("Pump stopped: ");
  Serial.println(reason);
}

bool startPump(uint32_t requestedMs, bool manual) {
  resetDailyBudgetIfNeeded();
  if (pumpRunning || latestSoilPercent < 0 || latestSoilRaw < 100 || latestSoilRaw > 4000) {
    Serial.println("Pump start rejected: invalid sensor reading or pump state");
    return false;
  }
  if (dailyPumpRuntimeMs >= min(maxDailyPumpMs, HARD_MAX_DAILY_PUMP_MS)) {
    Serial.println("Pump start rejected: daily runtime limit reached");
    return false;
  }
  if (!manual && (!automaticWateringEnabled || latestSoilPercent > soilStartThreshold)) return false;
  if (!manual && hasWatered && millis() - lastWaterEndedAt < waterIntervalMs) {
    Serial.println("Pump start rejected: minimum watering interval not reached");
    return false;
  }

  uint32_t remaining = min(maxDailyPumpMs, HARD_MAX_DAILY_PUMP_MS) - dailyPumpRuntimeMs;
  uint32_t runTime = min(requestedMs, min(HARD_MAX_PUMP_RUN_MS, remaining));
  if (runTime == 0) return false;

  pumpStartedAt = millis();
  pumpStopAt = pumpStartedAt + runTime;
  pumpRunning = true;
  hasWatered = true;
  digitalWrite(PUMP_PIN, HIGH);
  Serial.print("Pump on for ");
  Serial.print(runTime);
  Serial.println(" ms");
  return true;
}

void readSensors() {
  latestLux = lightSensorReady ? lightMeter.readLightLevel() : NAN;
  latestTemperature = sht31Ready ? sht31.readTemperature() : NAN;
  latestHumidity = sht31Ready ? sht31.readHumidity() : NAN;
  latestSoilRaw = readSoilRaw();
  latestSoilPercent = soilPercentFromRaw(latestSoilRaw);

  Serial.print("Light: ");
  if (isnan(latestLux)) Serial.print("unavailable"); else Serial.print(latestLux, 1);
  Serial.print(" lx | Air: ");
  if (isnan(latestTemperature)) Serial.print("unavailable"); else Serial.print(latestTemperature, 1);
  Serial.print(" C, ");
  if (isnan(latestHumidity)) Serial.print("unavailable"); else Serial.print(latestHumidity, 1);
  Serial.print("% RH | Soil: ");
  Serial.print(latestSoilRaw);
  Serial.print(" raw, ");
  Serial.print(latestSoilPercent);
  Serial.println("%");
}

void servicePump() {
  if (pumpRunning && static_cast<int32_t>(millis() - pumpStopAt) >= 0) {
    stopPump("pulse complete");
  }
  if (pumpRunning && millis() - pumpStartedAt >= HARD_MAX_PUMP_RUN_MS) {
    stopPump("maximum runtime safety cutoff");
  }
}

void updateScheduledLight() {
  bool lightOn = false;
  if (manualLightMode) {
    lightOn = manualLightOn;
  } else if (automaticLightEnabled && lightSensorReady && isfinite(latestLux)) {
    if (!automaticLightOn && latestLux <= lightLuxThreshold) automaticLightOn = true;
    if (automaticLightOn && latestLux >= lightLuxThreshold + lightLuxHysteresis) automaticLightOn = false;
    lightOn = automaticLightOn;
  } else if (scheduleEnabled && time(nullptr) > 1700000000) {
    time_t currentTime = time(nullptr);
    struct tm localTime;
    localtime_r(&currentTime, &localTime);
    int minuteOfDay = localTime.tm_hour * 60 + localTime.tm_min;
    if (lightOnMinute < lightOffMinute) {
      lightOn = minuteOfDay >= lightOnMinute && minuteOfDay < lightOffMinute;
    } else {
      lightOn = minuteOfDay >= lightOnMinute || minuteOfDay < lightOffMinute;
    }
  }
  ledcWrite(LIGHT_PIN, lightOn ? lightDuty : 0);
}

String jsonValue(const String &body, const char *key) {
  String marker = String("\"") + key + "\":";
  int start = body.indexOf(marker);
  if (start < 0) return "";
  start += marker.length();
  while (start < body.length() && body[start] == ' ') ++start;
  if (start < body.length() && body[start] == '\"') {
    ++start;
    int end = body.indexOf('\"', start);
    return end < 0 ? "" : body.substring(start, end);
  }
  int end = body.indexOf(',', start);
  if (end < 0) end = body.indexOf('}', start);
  return body.substring(start, end < 0 ? body.length() : end);
}

int jsonInt(const String &body, const char *key, int fallback) {
  String value = jsonValue(body, key);
  return value.length() ? value.toInt() : fallback;
}

bool jsonBool(const String &body, const char *key, bool fallback) {
  String value = jsonValue(body, key);
  if (value == "true") return true;
  if (value == "false") return false;
  return fallback;
}

void applyDeviceConfig(const String &body) {
  scheduleEnabled = jsonBool(body, "schedule_enabled", scheduleEnabled);
  lightOnMinute = constrain(jsonInt(body, "light_on_minute", lightOnMinute), 0, 1439);
  lightOffMinute = constrain(jsonInt(body, "light_off_minute", lightOffMinute), 0, 1439);
  lightDuty = constrain(jsonInt(body, "light_duty", lightDuty), 0, PWM_MAX);
  automaticLightEnabled = jsonBool(body, "auto_light_enabled", automaticLightEnabled);
  lightLuxThreshold = constrain(jsonInt(body, "light_lux_threshold", lightLuxThreshold), 0, 200000);
  lightLuxHysteresis = constrain(jsonInt(body, "light_lux_hysteresis", lightLuxHysteresis), 0, 200000);
  automaticWateringEnabled = jsonBool(body, "auto_water_enabled", automaticWateringEnabled);
  soilStartThreshold = constrain(jsonInt(body, "soil_start_threshold", soilStartThreshold), 0, 100);
  waterPulseMs = constrain(jsonInt(body, "water_pulse_ms", waterPulseMs), 250, HARD_MAX_PUMP_RUN_MS);
  waterIntervalMs = constrain(jsonInt(body, "water_interval_ms", waterIntervalMs), 10000, 86400000);
  maxDailyPumpMs = constrain(jsonInt(body, "max_daily_pump_ms", maxDailyPumpMs), 1000, HARD_MAX_DAILY_PUMP_MS);
}

void addDeviceHeaders(HTTPClient &http) {
  http.addHeader("X-Device-Token", DEVICE_TOKEN);
  http.setTimeout(2500);
}

void maintainWifi() {
  if (WIFI_SSID[0] == '\0' || WiFi.status() == WL_CONNECTED) return;
  if (lastWifiAttemptAt != 0 && millis() - lastWifiAttemptAt < WIFI_RETRY_INTERVAL_MS) return;
  lastWifiAttemptAt = millis();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

String numberOrNull(float value) {
  return isnan(value) || isinf(value) ? String("null") : String(value, 2);
}

void uploadTelemetry() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(BACKEND_URL) + "/api/sensors");
  addDeviceHeaders(http);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"device_id\":\"" + String(DEVICE_ID) + "\",\"air_temperature\":" + numberOrNull(latestTemperature);
  body += ",\"air_humidity\":" + numberOrNull(latestHumidity) + ",\"light_lux\":" + numberOrNull(latestLux);
  body += ",\"soil_moisture_raw\":" + String(latestSoilRaw) + ",\"soil_moisture_percent\":" + String(latestSoilPercent);
  body += ",\"pump_running\":" + String(pumpRunning ? "true" : "false");
  body += ",\"light_duty\":" + String(ledcRead(LIGHT_PIN)) + "}";
  int status = http.POST(body);
  if (status < 200 || status >= 300) Serial.printf("Telemetry HTTP error: %d\n", status);
  http.end();
}

void reportCommandResult(int commandId, bool success, const char *message) {
  if (WiFi.status() != WL_CONNECTED || commandId <= 0) return;
  if (pumpRunning) {
    pendingCommandResultId = commandId;
    pendingCommandResultSuccess = success;
    pendingCommandResultMessage = message;
    return;
  }
  HTTPClient http;
  String url = String(BACKEND_URL) + "/api/device/commands/" + String(commandId) + "/result";
  http.begin(url);
  addDeviceHeaders(http);
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"result\":\"") + (success ? "ok" : "failed") + "\",\"message\":\"" + message + "\"}";
  http.POST(body);
  http.end();
}

void flushCommandResult() {
  if (pendingCommandResultId == 0 || pumpRunning || WiFi.status() != WL_CONNECTED) return;
  int commandId = pendingCommandResultId;
  pendingCommandResultId = 0;
  reportCommandResult(commandId, pendingCommandResultSuccess, pendingCommandResultMessage.c_str());
}

void pollBackend() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String(BACKEND_URL) + "/api/device/poll?device_id=" + DEVICE_ID;
  http.begin(url);
  addDeviceHeaders(http);
  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    if (status > 0) Serial.printf("Device poll HTTP error: %d\n", status);
    http.end();
    return;
  }
  String body = http.getString();
  http.end();
  applyDeviceConfig(body);

  if (body.indexOf("\"command\":null") >= 0) return;
  int commandId = jsonInt(body, "id", 0);
  String action = jsonValue(body, "action");
  bool success = false;
  const char *message = "unsupported command";
  if (action == "pump_test") {
    uint32_t duration = constrain(jsonInt(body, "duration_ms", 2000), 250, HARD_MAX_PUMP_RUN_MS);
    success = startPump(duration, true);
    message = success ? "pump pulse started" : "pump safety check rejected command";
  } else if (action == "light_on") {
    manualLightMode = true;
    manualLightOn = true;
    updateScheduledLight();
    success = true;
    message = "light turned on";
  } else if (action == "light_off") {
    manualLightMode = true;
    manualLightOn = false;
    updateScheduledLight();
    success = true;
    message = "light turned off";
  } else if (action == "light_auto") {
    manualLightMode = false;
    updateScheduledLight();
    success = true;
    message = "automatic light schedule restored";
  }
  reportCommandResult(commandId, success, message);
}

void handleSerialCommand(String command) {
  command.trim();
  command.toUpperCase();

  if (command == "SENSORS") {
    readSensors();
  } else if (command == "STATUS") {
    Serial.print("Auto watering: "); Serial.println(automaticWateringEnabled ? "ON" : "OFF");
    Serial.print("Pump: "); Serial.println(pumpRunning ? "ON" : "OFF");
    Serial.print("Light control: "); Serial.println(manualLightMode ? "MANUAL" : "SCHEDULE");
    Serial.print("Soil: "); Serial.print(latestSoilRaw); Serial.print(" raw / "); Serial.print(latestSoilPercent); Serial.println("%");
    Serial.print("Pump runtime in current 24h window: "); Serial.print(dailyPumpRuntimeMs); Serial.println(" ms");
  } else if (command == "PUMP TEST") {
    startPump(2000, true);
  } else if (command == "PUMP STOP") {
    stopPump("serial command");
  } else if (command == "AUTO ON") {
    automaticWateringEnabled = true;
    Serial.println("Automatic watering enabled");
  } else if (command == "AUTO OFF") {
    automaticWateringEnabled = false;
    stopPump("automatic watering disabled");
    Serial.println("Automatic watering disabled");
  } else if (command == "LIGHT ON") {
    manualLightMode = true;
    manualLightOn = true;
    updateScheduledLight();
  } else if (command == "LIGHT OFF") {
    manualLightMode = true;
    manualLightOn = false;
    updateScheduledLight();
  } else if (command == "LIGHT AUTO") {
    manualLightMode = false;
    updateScheduledLight();
  } else if (command == "HELP") {
    Serial.println("Commands: SENSORS, STATUS, PUMP TEST, PUMP STOP, AUTO ON, AUTO OFF, LIGHT ON, LIGHT OFF, LIGHT AUTO");
  } else if (command.length() > 0) {
    Serial.println("Unknown command. Type HELP.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  pinMode(SOIL_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  ledcAttach(LIGHT_PIN, PWM_FREQUENCY, PWM_RESOLUTION);
  ledcWrite(LIGHT_PIN, 0);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lightSensorReady = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  sht31Ready = sht31.begin(0x44);

  if (WIFI_SSID[0] != '\0') {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  }

  Serial.println("Plant cabinet firmware ready");
  Serial.print("BH1750: "); Serial.println(lightSensorReady ? "ready" : "not found");
  Serial.print("SHT31: "); Serial.println(sht31Ready ? "ready" : "not found");
  Serial.println("Pump and light start OFF. Automatic watering starts disabled.");
  Serial.println("Type HELP in Serial Monitor. Set line ending to Newline.");
  readSensors();
}

void loop() {
  if (Serial.available()) {
    handleSerialCommand(Serial.readStringUntil('\n'));
  }

  uint32_t now = millis();
  maintainWifi();
  if (lastSensorAt == 0 || now - lastSensorAt >= SENSOR_INTERVAL_MS) {
    lastSensorAt = now;
    readSensors();
    if (automaticWateringEnabled && latestSoilRaw >= 100 && latestSoilRaw <= 4000 &&
        latestSoilPercent <= soilStartThreshold &&
        (lastAutoWaterAttemptAt == 0 || now - lastAutoWaterAttemptAt >= waterIntervalMs)) {
      lastAutoWaterAttemptAt = now;
      startPump(waterPulseMs, false);
    }
  }

  servicePump();
  updateScheduledLight();
  flushCommandResult();
  if (!pumpRunning && now - lastTelemetryAt >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryAt = now;
    uploadTelemetry();
  }
  if (!pumpRunning && now - lastDevicePollAt >= DEVICE_POLL_INTERVAL_MS) {
    lastDevicePollAt = now;
    pollBackend();
  }
  delay(5);
}
