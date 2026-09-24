/*
 * ESP32 Smart Irrigation System
 * Capacitive soil moisture sensor, pump via relay, state machine with
 * hysteresis and safety shutdown, web dashboard, watering history (NTP time),
 * MQTT telemetry and over-the-air (OTA) updates.
 * Author: Khaled Abu Mustafa, 2026 – MIT License
 */

#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <time.h>

// WiFi and OTA credentials live in secrets.h (not uploaded, see secrets.example.h)
#include "secrets.h"

const char* PLANT_NAME = "Feri";

// ==================================================
// Pins
// ==================================================

const int SENSOR_PIN = 35;           // analog output of the moisture sensor (ADC1)
const int RELAY_PIN  = 5;            // relay control signal
const int PUMP_ON    = LOW;          // relay module is active LOW
const int PUMP_OFF   = HIGH;

// ==================================================
// Sensor calibration
// ==================================================

const int RAW_DRY = 2680;            // raw value in air
const int RAW_WET = 1300;            // raw value in water
const int RAW_MIN_VALID = 500;       // outside this range: broken wire or short circuit
const int RAW_MAX_VALID = 3500;

// ==================================================
// Control parameters
// ==================================================

const int START_MOISTURE = 40;                  // % -> start watering
const int STOP_MOISTURE  = 60;                  // % -> back to IDLE (hysteresis)
const unsigned long PUMP_TIME_MS = 3000;        // pump run time per cycle
const unsigned long SOAK_TIME_MS = 60000;       // wait until the water has spread
const int MAX_CYCLES_PER_HOUR = 5;              // protection against flooding / dry running
const unsigned long CONTROL_PERIOD_MS = 100;    // control loop period

// ==================================================
// Time (NTP)
// ==================================================

const char* NTP_SERVER = "pool.ntp.org";
const char* TIMEZONE   = "CET-1CEST,M3.5.0,M10.5.0/3";   // Germany incl. daylight saving time

// ==================================================
// MQTT
// ==================================================

const char* MQTT_SERVER = "test.mosquitto.org";   // public test broker
const int   MQTT_PORT   = 1883;
const char* TOPIC_MOISTURE = "irrigation/esp32/moisture";
const char* TOPIC_STATE    = "irrigation/esp32/state";
const unsigned long MQTT_PUBLISH_MS = 5000;
const unsigned long MQTT_RETRY_MS   = 5000;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebServer server(80);

// ==================================================
// State machine
// ==================================================

enum State { IDLE, WATERING, SOAKING, FAULT };
State state = IDLE;

unsigned long stateSince = 0;         // when the current state started
unsigned long hourWindowStart = 0;    // start of the current one-hour window
int cyclesThisHour = 0;
String faultText = "";

int rawValue = 0;
int moisture = 0;

unsigned long lastControl = 0;
unsigned long lastMqttPublish = 0;
unsigned long lastMqttAttempt = 0;
bool otaStarted = false;

// ==================================================
// Watering history
// ==================================================

struct WateringEvent {
  char time[20];      // "dd.mm.yyyy hh:mm"
  int before;         // moisture before watering (%)
  int after;          // moisture after the soak time (%), -1 = still open
};

const int HISTORY_SIZE = 10;
WateringEvent history[HISTORY_SIZE];
int historyCount = 0;


// ==================================================
// Helper functions
// ==================================================

const char* stateName() {
  switch (state) {
    case IDLE:     return "IDLE";
    case WATERING: return "WATERING";
    case SOAKING:  return "SOAKING";
    case FAULT:    return "FAULT";
  }
  return "UNKNOWN";
}

const char* moistureLabel(int m) {
  if (m <= 20) return "Very dry";
  if (m <= 40) return "Dry";
  if (m <= 70) return "Moist";
  if (m <= 90) return "Very moist";
  return "Wet";
}

void changeState(State next) {
  state = next;
  stateSince = millis();
  Serial.print("State -> ");
  Serial.println(stateName());
}

// Average of 16 readings so single outliers do not trigger the pump
int readRaw() {
  long sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogRead(SENSOR_PIN);
  }
  return sum / 16;
}

int toPercent(int raw) {
  int p = map(raw, RAW_DRY, RAW_WET, 0, 100);
  return constrain(p, 0, 100);
}

void currentTime(char* buffer, size_t length) {
  struct tm t;
  if (getLocalTime(&t, 10)) {
    strftime(buffer, length, "%d.%m.%Y %H:%M", &t);
  } else {
    snprintf(buffer, length, "time unavailable");
  }
}

void addHistoryEntry(int before) {
  // Drop the oldest entry when the list is full
  if (historyCount == HISTORY_SIZE) {
    for (int i = 0; i < HISTORY_SIZE - 1; i++) {
      history[i] = history[i + 1];
    }
    historyCount--;
  }
  WateringEvent& e = history[historyCount++];
  currentTime(e.time, sizeof(e.time));
  e.before = before;
  e.after = -1;
}

void completeLastHistoryEntry(int after) {
  if (historyCount > 0 && history[historyCount - 1].after < 0) {
    history[historyCount - 1].after = after;
  }
}

void enterFault(const String& reason) {
  digitalWrite(RELAY_PIN, PUMP_OFF);
  faultText = reason;
  Serial.println("FAULT: " + reason);
  changeState(FAULT);
}

void startPump() {
  // Reset the one-hour window when it has expired
  if (millis() - hourWindowStart >= 3600000UL) {
    hourWindowStart = millis();
    cyclesThisHour = 0;
  }

  if (cyclesThisHour >= MAX_CYCLES_PER_HOUR) {
    enterFault("Too many watering cycles per hour (tank empty or sensor not in soil?)");
    return;
  }

  cyclesThisHour++;
  addHistoryEntry(moisture);
  digitalWrite(RELAY_PIN, PUMP_ON);
  changeState(WATERING);
}


// ==================================================
// Control logic
// ==================================================

void updateControl() {
  rawValue = readRaw();
  moisture = toPercent(rawValue);

  // Safety first: implausible sensor value -> pump off
  if (state != FAULT && (rawValue < RAW_MIN_VALID || rawValue > RAW_MAX_VALID)) {
    enterFault("Implausible sensor value (" + String(rawValue) + ")");
    return;
  }

  switch (state) {

    case IDLE:
      if (moisture <= START_MOISTURE) {
        startPump();
      }
      break;

    case WATERING:
      if (millis() - stateSince >= PUMP_TIME_MS) {
        digitalWrite(RELAY_PIN, PUMP_OFF);
        changeState(SOAKING);
      }
      break;

    case SOAKING:
      // Evaluate only after the water has had time to spread
      if (millis() - stateSince >= SOAK_TIME_MS) {
        completeLastHistoryEntry(moisture);
        if (moisture >= STOP_MOISTURE) {
          changeState(IDLE);
        } else {
          startPump();          // still too dry -> next watering pulse
        }
      }
      break;

    case FAULT:
      // Pump stays off until the fault is acknowledged on the web dashboard
      digitalWrite(RELAY_PIN, PUMP_OFF);
      break;
  }
}


// ==================================================
// MQTT (non-blocking)
// ==================================================

void maintainMqtt() {
  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }

  // Retry only every few seconds so the control loop is never blocked
  if (WiFi.status() != WL_CONNECTED || millis() - lastMqttAttempt < MQTT_RETRY_MS) {
    return;
  }
  lastMqttAttempt = millis();

  String clientId = "ESP32-Irrigation-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(clientId.c_str())) {
    Serial.println("MQTT connected");
  } else {
    Serial.print("MQTT error, state: ");
    Serial.println(mqtt.state());
  }
}

void publishMqtt() {
  if (!mqtt.connected()) return;
  mqtt.publish(TOPIC_MOISTURE, String(moisture).c_str());
  mqtt.publish(TOPIC_STATE, stateName());
}


// ==================================================
// OTA updates
// ==================================================

void maintainOta() {
  if (!otaStarted && WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.setHostname("smart-irrigation");
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { digitalWrite(RELAY_PIN, PUMP_OFF); });   // pump off during update
    ArduinoOTA.begin();
    otaStarted = true;
    Serial.println("OTA ready");
  }
  if (otaStarted) {
    ArduinoOTA.handle();
  }
}


// ==================================================
// Web dashboard
// ==================================================

const char DASHBOARD[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Irrigation</title>
<style>
  :root { --bg:#f4f4f4; --card:#fff; --text:#222; --muted:#777; --line:#ddd; --blue:#2e86de; --red:#c0392b; }
  @media (prefers-color-scheme: dark) {
    :root { --bg:#111; --card:#1c1c1c; --text:#eee; --muted:#999; --line:#333; }
  }
  body { font-family: system-ui, Arial, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 16px; }
  .wrap { max-width: 520px; margin: 0 auto; }
  h1 { font-size: 22px; margin: 4px 0 16px; }
  .card { background: var(--card); border: 1px solid var(--line); border-radius: 10px; padding: 16px; margin-bottom: 12px; }
  .label { font-size: 13px; color: var(--muted); }
  .big { font-size: 38px; font-weight: 600; }
  .bar { height: 12px; background: var(--line); border-radius: 6px; overflow: hidden; margin: 8px 0 6px; }
  .fill { height: 100%; width: 0; background: var(--blue); transition: width .5s; }
  .state { font-size: 22px; font-weight: 600; }
  .FAULT { color: var(--red); }
  .WATERING { color: var(--blue); }
  #fault { color: var(--red); margin-top: 6px; }
  button { margin-top: 10px; padding: 8px 14px; font-size: 15px; border-radius: 6px; border: 1px solid var(--line); background: var(--card); color: var(--text); cursor: pointer; }
  ul { list-style: none; padding: 0; margin: 6px 0 0; }
  li { padding: 6px 0; border-bottom: 1px solid var(--line); font-size: 14px; }
  li:last-child { border-bottom: none; }
  .small { font-size: 13px; color: var(--muted); }
</style>
</head>
<body>
<div class="wrap">
  <h1>Smart Irrigation · <span id="plant"></span></h1>

  <div class="card">
    <div class="label">Soil moisture</div>
    <div class="big"><span id="moisture">--</span> %</div>
    <div class="bar"><div class="fill" id="fill"></div></div>
    <div class="small"><span id="status">--</span> · raw value <span id="raw">--</span></div>
  </div>

  <div class="card">
    <div class="label">System state</div>
    <div class="state" id="state">--</div>
    <div id="fault"></div>
    <button id="reset" style="display:none" onclick="fetch('/reset').then(update)">Acknowledge fault</button>
    <div class="small" style="margin-top:8px">Watering cycles this hour: <span id="cycles">--</span></div>
  </div>

  <div class="card">
    <div class="label">Watering history</div>
    <ul id="history"><li>No watering recorded yet.</li></ul>
  </div>
</div>
<script>
  function update() {
    fetch('/sensor').then(r => r.json()).then(d => {
      document.getElementById('plant').textContent = d.plant;
      document.getElementById('moisture').textContent = d.moisture;
      document.getElementById('fill').style.width = d.moisture + '%';
      document.getElementById('status').textContent = d.status;
      document.getElementById('raw').textContent = d.raw;
      const s = document.getElementById('state');
      s.textContent = d.state;
      s.className = 'state ' + d.state;
      document.getElementById('fault').textContent = d.fault;
      document.getElementById('reset').style.display = d.state === 'FAULT' ? 'inline-block' : 'none';
      document.getElementById('cycles').textContent = d.cycles;

      const list = document.getElementById('history');
      list.innerHTML = '';
      if (d.history.length === 0) {
        list.innerHTML = '<li>No watering recorded yet.</li>';
      }
      d.history.slice().reverse().forEach(e => {
        const li = document.createElement('li');
        const after = e.after < 0 ? 'soaking…' : e.after + ' %';
        li.textContent = e.time + ' · ' + e.before + ' % → ' + after;
        list.appendChild(li);
      });
    }).catch(() => {});
  }
  setInterval(update, 1000);
  update();
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", DASHBOARD);
}

void handleSensor() {
  String json = "{";
  json += "\"plant\":\"" + String(PLANT_NAME) + "\",";
  json += "\"moisture\":" + String(moisture) + ",";
  json += "\"raw\":" + String(rawValue) + ",";
  json += "\"status\":\"" + String(moistureLabel(moisture)) + "\",";
  json += "\"state\":\"" + String(stateName()) + "\",";
  json += "\"fault\":\"" + faultText + "\",";
  json += "\"cycles\":" + String(cyclesThisHour) + ",";
  json += "\"history\":[";
  for (int i = 0; i < historyCount; i++) {
    if (i > 0) json += ",";
    json += "{\"time\":\"" + String(history[i].time) + "\"";
    json += ",\"before\":" + String(history[i].before);
    json += ",\"after\":" + String(history[i].after) + "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleReset() {
  if (state == FAULT) {
    faultText = "";
    cyclesThisHour = 0;
    hourWindowStart = millis();
    changeState(IDLE);
  }
  server.send(200, "text/plain", "OK");
}


// ==================================================
// Setup and loop
// ==================================================

void setup() {
  Serial.begin(115200);

  // Set the relay to OFF before enabling the output so the pump never starts on boot
  digitalWrite(RELAY_PIN, PUMP_OFF);
  pinMode(RELAY_PIN, OUTPUT);

  // Wait at most 20 s for WiFi; irrigation must also work without a network
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nIP address: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nNo WiFi - control keeps running, reconnecting in the background");
  }

  configTzTime(TIMEZONE, NTP_SERVER);

  server.on("/", handleRoot);
  server.on("/sensor", handleSensor);
  server.on("/reset", handleReset);
  server.begin();

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);

  hourWindowStart = millis();
  changeState(IDLE);
}

void loop() {
  // Control runs every 100 ms, independent of WiFi, MQTT and web requests
  if (millis() - lastControl >= CONTROL_PERIOD_MS) {
    lastControl = millis();
    updateControl();
  }

  server.handleClient();
  maintainMqtt();
  maintainOta();

  if (millis() - lastMqttPublish >= MQTT_PUBLISH_MS) {
    lastMqttPublish = millis();
    publishMqtt();
  }
}
