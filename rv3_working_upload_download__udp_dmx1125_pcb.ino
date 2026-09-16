/*
  DMX Web Sequencer for ESP32-S3 - FULLY FIXED VERSION
  All features working: Save, Load, Add, Delete, Start, Reset
*/

#include <Arduino.h>
#include <algorithm>
#include <vector>
#include <esp_dmx.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>

// ===================== CONFIGURATION =====================
const char* WIFI_SSID = "Integration";
const char* WIFI_PASSWORD = "Gmepass12345";

const char* AP_SSID = "DMX-SEQUENCER";
const char* AP_PASSWORD = "";

// ===================== PINS =====================
//#define DMX_PORT 2  // UART2 - UART1 (port 1) fails to install on this board
const dmx_port_t DMX_PORT = DMX_NUM_1;
#define DMX_TX_PIN 17
#define DMX_RX_PIN 18
#define DMX_EN_PIN 13

#define SDA_PIN 35
#define SCL_PIN 36
#define OLED_RST -1

#define BTN_START 14
#define BTN_STOP 46

// ===================== CONSTANTS =====================
#define DMX_PACKET_SIZE 513
#define DMX_UPDATE_INTERVAL 25

// ===================== GLOBAL VARIABLES =====================
byte dmxData[DMX_PACKET_SIZE];
byte lastDMXValues[DMX_PACKET_SIZE];
bool dmxOK = false;

Adafruit_SH1106G display(128, 64, &Wire, OLED_RST);
bool oledOK = false;

WebServer server(80);
bool wifiConnected = false;

enum SystemState {
  STATE_IDLE,
  STATE_RUNNING
};
SystemState currentState = STATE_IDLE;
unsigned long showStartTime = 0;
unsigned long elapsedSeconds = 0;

struct Config {
  char productTitle[32] = "DMX CONTROLLER";
  uint32_t showDuration = 300;
  uint16_t fadeDuration = 2;
  char udpStartMsg[32] = "startdmx";
  char udpStopMsg[32] = "stopdmx";
  uint16_t udpPort = 10000;
} config;

// ===================== UDP =====================
WiFiUDP udp;
char udpIncomingPacket[64];

struct Cue {
  uint32_t id;
  uint32_t time;
  uint16_t channel;
  bool fadeIn;
  bool fadeOut;
  uint8_t rowValue;
  uint8_t startValue;
  uint8_t endValue;
  bool active;
};

std::vector<Cue> idleCues;
std::vector<Cue> mainCues;
uint32_t nextCueId = 1;

struct ActiveCue {
  uint32_t cueId;
  uint16_t channel;
  bool fadeIn;
  bool fadeOut;
  uint8_t startValue;
  uint8_t endValue;
  uint32_t startTime;
  uint32_t duration;
  bool completed;
};
std::vector<ActiveCue> activeFades;

unsigned long lastDMXUpdate = 0;
unsigned long lastOLEDUpdate = 0;
unsigned long lastStartPress = 0;
unsigned long lastStopPress = 0;

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== DMX Web Sequencer ===");
  Serial.println("Starting up...");

  setupSPIFFS();
  setupOLED();
  setupDMX();
  setupButtons();
  setupWiFi();
  setupMDNS();
  setupWebServer();
  loadDataFromSPIFFS();
  setupUDP();

  memset(dmxData, 0, DMX_PACKET_SIZE);
  memset(lastDMXValues, 0, DMX_PACKET_SIZE);
  processIdleCues();
  updateOLED();

  Serial.println("Setup complete!");
  if (wifiConnected) {
    Serial.print("Access the web interface at: http://");
    Serial.println(WiFi.localIP().toString());
    Serial.println("  or http://dmx.local");
  } else {
    Serial.print("Access the web interface at: http://");
    Serial.println(WiFi.softAPIP().toString());
  }
}

void loop() {
  server.handleClient();
  checkButtons();
  checkUDP();

  if (millis() - lastDMXUpdate >= DMX_UPDATE_INTERVAL) {
    lastDMXUpdate = millis();
    updateDMX();
  }

  if (millis() - lastOLEDUpdate >= 500) {
    lastOLEDUpdate = millis();
    updateOLED();
  }

  if (currentState == STATE_RUNNING) {
    elapsedSeconds = (millis() - showStartTime) / 1000;
    if (elapsedSeconds >= config.showDuration) {
      Serial.println("Show finished!");
      resetShow();
    }
  }
}

// ===================== SETUP FUNCTIONS =====================

void setupSPIFFS() {
  Serial.print("Mounting SPIFFS... ");
  if (!SPIFFS.begin(true)) {
    Serial.println("FAILED!");
    if (SPIFFS.format()) {
      Serial.println("Format success!");
      SPIFFS.begin(true);
    }
  } else {
    Serial.println("OK");
  }
}

void setupOLED() {
  Serial.print("Initializing OLED... ");
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!display.begin(0x3C, true)) {
    Serial.println("FAILED!");
    oledOK = false;
    return;
  }
  display.clearDisplay();
  display.drawRect(0, 0, 128, 64, SH110X_WHITE);
  display.setTextSize(2);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(50, 0);
  display.println("DMX");  //Sequencer
  display.setCursor(10, 22);
  display.println("Sequencer");  //Sequencer
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(25, 45);
  display.println("Starting...");
  display.display();
  oledOK = true;
  Serial.println("OK");
}


void setupDMX() {
  Serial.println("Initializing DMX...");

  const dmx_port_t port = DMX_PORT;

  Serial.printf(
    " DMX port: %d, TX: GPIO%d, RX: GPIO%d, RTS/EN: GPIO%d\n",
    (int)port,
    DMX_TX_PIN,
    DMX_RX_PIN,
    DMX_EN_PIN);

  // Remove a driver only if this application already installed one.
  if (dmx_driver_is_installed(port)) {
    Serial.println(" Existing DMX driver found; deleting it...");
    dmx_driver_delete(port);
  }

  dmx_config_t dmxConfig = DMX_CONFIG_DEFAULT;

  // esp_dmx v4.1 in this library returns bool, not esp_err_t.
  bool installOK = dmx_driver_install(
    port,
    &dmxConfig,
    nullptr,
    0);

  if (!installOK) {
    Serial.println(" FAILED! dmx_driver_install() returned false.");
    Serial.println(" DMX will be disabled.");
    dmxOK = false;
    return;
  }

  Serial.println(" Driver installed.");

  // dmx_set_pin() also returns bool in this library.
  bool pinOK = dmx_set_pin(
    port,
    DMX_TX_PIN,
    DMX_RX_PIN,
    DMX_EN_PIN);

  if (!pinOK) {
    Serial.println(" FAILED! dmx_set_pin() returned false.");
    dmx_driver_delete(port);
    dmxOK = false;
    return;
  }

  dmxOK = true;

  Serial.println(" DMX initialized successfully.");
}

void setupButtons() {
  Serial.print("Setting up buttons... ");
  pinMode(BTN_START, INPUT_PULLUP);
  pinMode(BTN_STOP, INPUT_PULLUP);
  Serial.println("OK");
}

void setupWiFi() {
  Serial.println("Setting up WiFi...");

  if (strlen(WIFI_SSID) > 0) {
    Serial.print("Connecting to ");
    Serial.print(WIFI_SSID);
    Serial.print("... ");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(" OK!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      wifiConnected = true;
      return;
    } else {
      Serial.println(" FAILED!");
    }
  }

  Serial.println("Starting AP mode...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  wifiConnected = false;
}

void setupMDNS() {
  Serial.print("Setting up mDNS... ");
  if (MDNS.begin("dmx")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("OK");
  } else {
    Serial.println("FAILED!");
  }
}

void setupUDP() {
  udp.begin(config.udpPort);
  Serial.print("UDP listener started on port ");
  Serial.println(config.udpPort);
  Serial.print("  Start message: ");
  Serial.println(config.udpStartMsg);
  Serial.print("  Stop/Reset message: ");
  Serial.println(config.udpStopMsg);
}

void restartUDP() {
  udp.stop();
  setupUDP();
}

void checkUDP() {
  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  int len = udp.read(udpIncomingPacket, sizeof(udpIncomingPacket) - 1);
  udpIncomingPacket[(len > 0) ? len : 0] = '\0';

  String msg = String(udpIncomingPacket);
  msg.trim();
  msg.toLowerCase();

  if (msg.length() == 0) return;

  Serial.print("UDP message received: ");
  Serial.println(msg);

  if (msg == String(config.udpStartMsg)) {
    if (startShowAction()) {
      Serial.println("Show started via UDP");
    }
  } else if (msg == String(config.udpStopMsg)) {
    resetShow();
    Serial.println("Show reset via UDP");
  } else {
    Serial.println("UDP message did not match start/stop commands");
  }
}

void setupWebServer() {
  Serial.println("Setting up web server...");

  server.on("/", handleRoot);
  server.on("/idle", handleIdle);
  server.on("/api/cues", HTTP_GET, handleGetCues);
  server.on("/api/cues/add", HTTP_POST, handleAddCue);
  server.on("/api/cues/delete", HTTP_POST, handleDeleteCues);
  server.on("/api/cues/save", HTTP_POST, handleSaveCues);
  server.on("/api/start", HTTP_POST, handleStartShow);
  server.on("/api/reset", HTTP_POST, handleResetShow);
  server.on("/api/config", HTTP_POST, handleSaveConfig);
  server.on("/api/backup/idle", HTTP_GET, handleBackupIdle);
  server.on("/api/backup/main", HTTP_GET, handleBackupMain);
  server.on("/api/restore/idle", HTTP_POST, handleRestoreIdle);
  server.on("/api/restore/main", HTTP_POST, handleRestoreMain);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Web server started");
}

// ===================== HELPERS =====================
long clampLong(long val, long lo, long hi) {
  if (val < lo) return lo;
  if (val > hi) return hi;
  return val;
}

// ===================== WEB HANDLERS =====================

void handleRoot() {
  server.send(200, "text/html", generateHTML(false));
}

void handleIdle() {
  server.send(200, "text/html", generateHTML(true));
}

void handleGetCues() {
  DynamicJsonDocument doc(20000);
  JsonArray idleArray = doc.createNestedArray("idle");
  JsonArray mainArray = doc.createNestedArray("main");

  for (const auto& cue : idleCues) {
    JsonObject obj = idleArray.createNestedObject();
    obj["id"] = cue.id;
    obj["time"] = cue.time;
    obj["channel"] = cue.channel;
    obj["fadeIn"] = cue.fadeIn;
    obj["fadeOut"] = cue.fadeOut;
    obj["rowValue"] = cue.rowValue;
    obj["startValue"] = cue.startValue;
    obj["endValue"] = cue.endValue;
  }

  for (const auto& cue : mainCues) {
    JsonObject obj = mainArray.createNestedObject();
    obj["id"] = cue.id;
    obj["time"] = cue.time;
    obj["channel"] = cue.channel;
    obj["fadeIn"] = cue.fadeIn;
    obj["fadeOut"] = cue.fadeOut;
    obj["rowValue"] = cue.rowValue;
    obj["startValue"] = cue.startValue;
    obj["endValue"] = cue.endValue;
  }

  doc["config"]["productTitle"] = config.productTitle;
  doc["config"]["showDuration"] = config.showDuration;
  doc["config"]["fadeDuration"] = config.fadeDuration;
  doc["config"]["udpStartMsg"] = config.udpStartMsg;
  doc["config"]["udpStopMsg"] = config.udpStopMsg;
  doc["config"]["udpPort"] = config.udpPort;
  doc["state"] = currentState == STATE_RUNNING ? "running" : "idle";
  doc["elapsedTime"] = currentState == STATE_RUNNING ? (millis() - showStartTime) / 1000 : 0;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleAddCue() {
  if (server.hasArg("page") && server.hasArg("time") && server.hasArg("channel")) {
    String page = server.arg("page");
    uint32_t time = server.arg("time").toInt();
    uint16_t channel = clampLong(server.arg("channel").toInt(), 1, 512);
    bool fadeIn = server.hasArg("fadeIn") ? server.arg("fadeIn") == "true" : false;
    bool fadeOut = server.hasArg("fadeOut") ? server.arg("fadeOut") == "true" : false;
    uint8_t rowValue = server.hasArg("rowValue") ? clampLong(server.arg("rowValue").toInt(), 0, 255) : 0;
    uint8_t startValue = server.hasArg("startValue") ? clampLong(server.arg("startValue").toInt(), 0, 255) : 0;
    uint8_t endValue = server.hasArg("endValue") ? clampLong(server.arg("endValue").toInt(), 0, 255) : 255;

    Cue newCue;
    newCue.id = nextCueId++;
    newCue.time = time;
    newCue.channel = channel;
    newCue.fadeIn = fadeIn;
    newCue.fadeOut = fadeOut;
    newCue.rowValue = rowValue;
    newCue.startValue = startValue;
    newCue.endValue = endValue;
    newCue.active = false;

    if (page == "idle") {
      idleCues.push_back(newCue);
      std::sort(idleCues.begin(), idleCues.end(),
                [](const Cue& a, const Cue& b) {
                  return a.time < b.time;
                });
    } else {
      mainCues.push_back(newCue);
      std::sort(mainCues.begin(), mainCues.end(),
                [](const Cue& a, const Cue& b) {
                  return a.time < b.time;
                });
    }

    saveDataToSPIFFS();
    server.send(200, "application/json", "{\"success\":true}");
  } else {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing parameters\"}");
  }
}

void handleDeleteCues() {
  if (server.hasArg("page") && server.hasArg("ids")) {
    String page = server.arg("page");
    String idsStr = server.arg("ids");

    std::vector<uint32_t> idsToDelete;
    int start = 0;
    int commaPos;
    do {
      commaPos = idsStr.indexOf(',', start);
      String idStr = (commaPos == -1) ? idsStr.substring(start) : idsStr.substring(start, commaPos);
      idsToDelete.push_back(idStr.toInt());
      start = commaPos + 1;
    } while (commaPos != -1);

    if (page == "idle") {
      idleCues.erase(
        std::remove_if(idleCues.begin(), idleCues.end(),
                       [&](const Cue& cue) {
                         return std::find(idsToDelete.begin(), idsToDelete.end(), cue.id) != idsToDelete.end();
                       }),
        idleCues.end());
    } else {
      mainCues.erase(
        std::remove_if(mainCues.begin(), mainCues.end(),
                       [&](const Cue& cue) {
                         return std::find(idsToDelete.begin(), idsToDelete.end(), cue.id) != idsToDelete.end();
                       }),
        mainCues.end());
    }

    saveDataToSPIFFS();
    server.send(200, "application/json", "{\"success\":true}");
  } else {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing parameters\"}");
  }
}

void handleSaveCues() {
  if (server.hasArg("page") && server.hasArg("cues")) {
    String page = server.arg("page");
    String cuesJson = server.arg("cues");

    DynamicJsonDocument doc(200000);
    DeserializationError error = deserializeJson(doc, cuesJson);

    if (!error) {
      JsonArray cuesArray = doc.as<JsonArray>();

      if (page == "idle") {
        idleCues.clear();
        for (JsonObject obj : cuesArray) {
          Cue cue;
          cue.id = obj["id"] | nextCueId;
          if (cue.id >= nextCueId) nextCueId = cue.id + 1;
          cue.time = obj["time"] | 0;
          cue.channel = clampLong(obj["channel"] | 1, 1, 512);
          cue.fadeIn = obj["fadeIn"] | false;
          cue.fadeOut = obj["fadeOut"] | false;
          cue.rowValue = clampLong(obj["rowValue"] | 0, 0, 255);
          cue.startValue = clampLong(obj["startValue"] | 0, 0, 255);
          cue.endValue = clampLong(obj["endValue"] | 255, 0, 255);
          cue.active = false;
          idleCues.push_back(cue);
        }
      } else {
        mainCues.clear();
        for (JsonObject obj : cuesArray) {
          Cue cue;
          cue.id = obj["id"] | nextCueId;
          if (cue.id >= nextCueId) nextCueId = cue.id + 1;
          cue.time = obj["time"] | 0;
          cue.channel = clampLong(obj["channel"] | 1, 1, 512);
          cue.fadeIn = obj["fadeIn"] | false;
          cue.fadeOut = obj["fadeOut"] | false;
          cue.rowValue = clampLong(obj["rowValue"] | 0, 0, 255);
          cue.startValue = clampLong(obj["startValue"] | 0, 0, 255);
          cue.endValue = clampLong(obj["endValue"] | 255, 0, 255);
          cue.active = false;
          mainCues.push_back(cue);
        }
      }

      saveDataToSPIFFS();
      server.send(200, "application/json", "{\"success\":true}");
    } else {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
    }
  } else {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing parameters\"}");
  }
}

bool startShowAction() {
  if (currentState == STATE_IDLE) {
    currentState = STATE_RUNNING;
    showStartTime = millis();
    elapsedSeconds = 0;
    activeFades.clear();
    memset(dmxData, 0, DMX_PACKET_SIZE);
    memset(lastDMXValues, 0, DMX_PACKET_SIZE);
    Serial.println("Show started!");
    return true;
  }
  return false;
}

void handleStartShow() {
  if (startShowAction()) {
    server.send(200, "application/json", "{\"success\":true}");
  } else {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"Show already running\"}");
  }
}

void handleResetShow() {
  resetShow();
  server.send(200, "application/json", "{\"success\":true}");
}

void handleSaveConfig() {
  if (server.hasArg("productTitle")) {
    strncpy(config.productTitle, server.arg("productTitle").c_str(), sizeof(config.productTitle) - 1);
    config.productTitle[sizeof(config.productTitle) - 1] = '\0';
  }
  if (server.hasArg("showDuration")) {
    config.showDuration = server.arg("showDuration").toInt();
    if (config.showDuration < 1) config.showDuration = 1;
    if (config.showDuration > 86400) config.showDuration = 86400;
  }
  if (server.hasArg("fadeDuration")) {
    config.fadeDuration = server.arg("fadeDuration").toInt();
    if (config.fadeDuration < 1) config.fadeDuration = 1;
    if (config.fadeDuration > 60) config.fadeDuration = 60;
  }

  bool udpPortChanged = false;

  if (server.hasArg("udpStartMsg")) {
    String msg = server.arg("udpStartMsg");
    msg.trim();
    msg.toLowerCase();
    if (msg.length() == 0) msg = "startdmx";
    strncpy(config.udpStartMsg, msg.c_str(), sizeof(config.udpStartMsg) - 1);
    config.udpStartMsg[sizeof(config.udpStartMsg) - 1] = '\0';
  }

  if (server.hasArg("udpStopMsg")) {
    String msg = server.arg("udpStopMsg");
    msg.trim();
    msg.toLowerCase();
    if (msg.length() == 0) msg = "stopdmx";
    strncpy(config.udpStopMsg, msg.c_str(), sizeof(config.udpStopMsg) - 1);
    config.udpStopMsg[sizeof(config.udpStopMsg) - 1] = '\0';
  }

  if (server.hasArg("udpPort")) {
    long newPort = server.arg("udpPort").toInt();
    newPort = clampLong(newPort, 1, 65535);
    if ((uint16_t)newPort != config.udpPort) {
      config.udpPort = (uint16_t)newPort;
      udpPortChanged = true;
    }
  }

  saveDataToSPIFFS();
  updateOLED();

  if (udpPortChanged) {
    restartUDP();
  }

  server.send(200, "application/json", "{\"success\":true}");
}

void handleNotFound() {
  server.send(404, "text/plain", "404: Not Found");
}

// ===================== BACKUP / RESTORE (per-page JSON files) =====================
// NOTE: Internally, idle cues and main cues both live together in ONE file on
// SPIFFS ("/data.json"), just as two separate arrays inside that JSON file.
// These handlers below do NOT change that internal storage file - they only
// generate/accept SEPARATE downloadable JSON files for backup and restore,
// one for idle cues and one for main cues, so you can back up or reload each
// page independently from the browser.

void sendCueBackup(const String& page) {
  DynamicJsonDocument doc(200000);
  doc["type"] = (page == "idle") ? "idleCues" : "mainCues";
  doc["page"] = page;
  doc["productTitle"] = config.productTitle;
  doc["exportedAt"] = millis();

  JsonArray arr = doc.createNestedArray("cues");
  const std::vector<Cue>& src = (page == "idle") ? idleCues : mainCues;
  for (const auto& cue : src) {
    JsonObject obj = arr.createNestedObject();
    obj["id"] = cue.id;
    obj["time"] = cue.time;
    obj["channel"] = cue.channel;
    obj["fadeIn"] = cue.fadeIn;
    obj["fadeOut"] = cue.fadeOut;
    obj["rowValue"] = cue.rowValue;
    obj["startValue"] = cue.startValue;
    obj["endValue"] = cue.endValue;
  }

  String response;
  serializeJson(doc, response);

  String filename = (page == "idle") ? "idle_cues_backup.json" : "main_cues_backup.json";
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", response);
}

void handleBackupIdle() {
  sendCueBackup("idle");
}

void handleBackupMain() {
  sendCueBackup("main");
}

void restoreCuesForPage(const String& page) {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"No file data received\"}");
    return;
  }

  String body = server.arg("plain");

  DynamicJsonDocument doc(200000);
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON file\"}");
    return;
  }

  JsonArray cuesArray;
  if (doc.is<JsonArray>()) {
    // Accept a plain array of cue objects too (no wrapper object)
    cuesArray = doc.as<JsonArray>();
  } else if (doc.containsKey("cues")) {
    // If the backup file records which page it came from, make sure it
    // matches the page being restored to, so idle/main files can't be
    // uploaded to the wrong page by mistake.
    if (doc.containsKey("type")) {
      String fileType = doc["type"].as<String>();
      String expectedType = (page == "idle") ? "idleCues" : "mainCues";
      if (fileType != expectedType) {
        String msg = "{\"success\":false,\"error\":\"This file is a " + fileType +
                     " backup, not a " + expectedType + " backup\"}";
        server.send(400, "application/json", msg);
        return;
      }
    }
    cuesArray = doc["cues"].as<JsonArray>();
  } else {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"JSON does not contain a cues array\"}");
    return;
  }

  std::vector<Cue> newCues;
  for (JsonObject obj : cuesArray) {
    Cue cue;
    cue.id = obj["id"] | nextCueId;
    if (cue.id >= nextCueId) nextCueId = cue.id + 1;
    cue.time = obj["time"] | 0;
    cue.channel = clampLong(obj["channel"] | 1, 1, 512);
    cue.fadeIn = obj["fadeIn"] | false;
    cue.fadeOut = obj["fadeOut"] | false;
    cue.rowValue = clampLong(obj["rowValue"] | 0, 0, 255);
    cue.startValue = clampLong(obj["startValue"] | 0, 0, 255);
    cue.endValue = clampLong(obj["endValue"] | 255, 0, 255);
    cue.active = false;
    newCues.push_back(cue);
  }

  std::sort(newCues.begin(), newCues.end(),
            [](const Cue& a, const Cue& b) { return a.time < b.time; });

  if (page == "idle") {
    idleCues = newCues;
    memset(dmxData, 0, DMX_PACKET_SIZE);
    processIdleCues();
  } else {
    mainCues = newCues;
  }

  saveDataToSPIFFS();

  String resp = "{\"success\":true,\"count\":" + String(newCues.size()) + "}";
  server.send(200, "application/json", resp);
}

void handleRestoreIdle() {
  restoreCuesForPage("idle");
}

void handleRestoreMain() {
  restoreCuesForPage("main");
}

// ===================== HTML GENERATOR =====================

String generateHTML(bool idlePage) {
  String ip = wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String safeTitle = String(config.productTitle);
  safeTitle.replace("&", "&amp;");
  safeTitle.replace("<", "&lt;");
  safeTitle.replace(">", "&gt;");
  safeTitle.replace("\"", "&quot;");
  safeTitle.replace("'", "&#39;");

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>queDMX Sencer</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: #1a1a2e;
            color: #e0e0e0;
            padding: 20px;
            min-height: 100vh;
        }
        .container { max-width: 1400px; margin: 0 auto; }
        h1 { 
            color: #00d4ff; 
            margin-bottom: 20px;
            text-align: center;
            font-size: 2em;
            text-shadow: 0 0 20px rgba(0,212,255,0.3);
        }
        .status-bar {
            background: #16213e;
            padding: 15px 20px;
            border-radius: 10px;
            margin-bottom: 20px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            flex-wrap: wrap;
            gap: 10px;
            border: 1px solid #0f3460;
        }
        .status-item {
            display: flex;
            align-items: center;
            gap: 10px;
        }
        .status-label { color: #888; font-size: 0.9em; }
        .status-value { 
            color: #00d4ff; 
            font-weight: bold;
            font-size: 1.1em;
        }
        .status-value.running { color: #00ff88; }
        .status-value.idle { color: #ffaa00; }
        .controls {
            display: flex;
            gap: 10px;
            flex-wrap: wrap;
            margin-bottom: 20px;
        }
        .btn {
            padding: 10px 20px;
            border: none;
            border-radius: 5px;
            cursor: pointer;
            font-weight: bold;
            transition: all 0.3s;
            font-size: 0.95em;
        }
        .btn:hover { transform: translateY(-2px); box-shadow: 0 5px 15px rgba(0,0,0,0.3); }
        .btn-start { background: #00ff88; color: #1a1a2e; }
        .btn-start:hover { background: #00cc6a; }
        .btn-start:disabled { opacity: 0.5; cursor: not-allowed; }
        .btn-reset { background: #ff4757; color: white; }
        .btn-reset:hover { background: #ff6b7a; }
        .btn-idle { background: #ffa502; color: #1a1a2e; }
        .btn-idle:hover { background: #ffbe44; }
        .btn-save { background: #2ed573; color: #1a1a2e; }
        .btn-save:hover { background: #7bed9f; }
        .btn-delete { background: #ff6348; color: white; }
        .btn-delete:hover { background: #ff7f6a; }
        .btn-add { background: #1e90ff; color: white; }
        .btn-add:hover { background: #4aa3ff; }
        .config-section {
            background: #16213e;
            padding: 15px 20px;
            border-radius: 10px;
            margin-bottom: 20px;
            display: flex;
            flex-wrap: wrap;
            gap: 20px;
            align-items: center;
            border: 1px solid #0f3460;
        }
        .config-group {
            display: flex;
            align-items: center;
            gap: 10px;
        }
        .config-group label { color: #888; font-size: 0.9em; }
        .config-group input {
            background: #0f3460;
            border: 1px solid #1a1a2e;
            color: #e0e0e0;
            padding: 8px 12px;
            border-radius: 5px;
            width: 150px;
        }
        .config-group input:focus {
            outline: none;
            border-color: #00d4ff;
        }
        .table-container {
            background: #16213e;
            padding: 20px;
            border-radius: 10px;
            overflow-x: auto;
            border: 1px solid #0f3460;
        }
        table {
            width: 100%;
            border-collapse: collapse;
            min-width: 950px;
        }
        th {
            background: #0f3460;
            color: #00d4ff;
            padding: 12px 8px;
            text-align: center;
            font-size: 0.85em;
            position: sticky;
            top: 0;
        }
        td {
            padding: 8px;
            text-align: center;
            border-bottom: 1px solid #1a1a2e;
            vertical-align: middle;
        }
        tr:hover { background: #1a1a2e; }
        .cue-number {
            color: #888;
            font-weight: bold;
            min-width: 30px;
        }
        .checkbox-input {
            width: 18px;
            height: 18px;
            cursor: pointer;
        }
        .time-input {
            background: #0f3460;
            border: 1px solid #1a1a2e;
            color: #e0e0e0;
            padding: 8px 6px;
            border-radius: 3px;
            width: 110px;
            font-size: 1em;
            text-align: center;
        }
        .channel-input {
            background: #0f3460;
            border: 1px solid #1a1a2e;
            color: #e0e0e0;
            padding: 8px 6px;
            border-radius: 3px;
            width: 75px;
            font-size: 1em;
            text-align: center;
        }
        .value-input {
            background: #0f3460;
            border: 1px solid #1a1a2e;
            color: #e0e0e0;
            padding: 8px 6px;
            border-radius: 3px;
            width: 75px;
            font-size: 1em;
            text-align: center;
        }
        .value-input:focus {
            outline: none;
            border-color: #00d4ff;
        }
        .delete-btn {
            background: none;
            border: none;
            color: #ff4757;
            font-size: 1.2em;
            cursor: pointer;
            padding: 4px 8px;
        }
        .delete-btn:hover { transform: scale(1.2); }
        .table-footer {
            margin-top: 15px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            flex-wrap: wrap;
            gap: 10px;
        }
        .total-cues {
            color: #888;
            font-size: 0.95em;
        }
        .total-cues span { color: #00d4ff; font-weight: bold; }
        .btn-group {
            display: flex;
            gap: 10px;
            flex-wrap: wrap;
        }
        .switch {
            position: relative;
            display: inline-block;
            width: 34px;
            height: 20px;
        }
        .switch input { opacity: 0; width: 0; height: 0; }
        .slider {
            position: absolute;
            cursor: pointer;
            top: 0;
            left: 0;
            right: 0;
            bottom: 0;
            background-color: #333;
            transition: .3s;
            border-radius: 20px;
        }
        .slider:before {
            position: absolute;
            content: "";
            height: 14px;
            width: 14px;
            left: 3px;
            bottom: 3px;
            background-color: white;
            transition: .3s;
            border-radius: 50%;
        }
        .switch input:checked + .slider { background-color: #00d4ff; }
        .switch input:checked + .slider:before { transform: translateX(14px); }
        .status-ip { color: #00d4ff; font-weight: bold; font-size: 0.9em; }
        .hidden { display: none; }
        .page-badge {
            text-align: center;
            padding: 10px;
            border-radius: 8px;
            margin-bottom: 20px;
            font-weight: bold;
            font-size: 1.1em;
            letter-spacing: 1px;
        }
        .page-badge.main-badge {
            background: rgba(0,212,255,0.15);
            border: 2px solid #00d4ff;
            color: #00d4ff;
        }
        .page-badge.idle-badge {
            background: rgba(255,165,2,0.15);
            border: 2px solid #ffa502;
            color: #ffa502;
        }
        body.idle-page h1 { color: #ffa502; text-shadow: 0 0 20px rgba(255,165,2,0.3); }
        @media (max-width: 768px) {
            .status-bar { flex-direction: column; align-items: stretch; }
            .controls { justify-content: center; }
            .config-section { flex-direction: column; align-items: stretch; }
            .config-group { justify-content: space-between; }
            .config-group input { width: 100%; }
        }
    </style>
</head>
<body class=")rawliteral"
                + String(idlePage ? "idle-page" : "main-page") + R"rawliteral(">
    <div class="container">
        <h1>)rawliteral"
                + safeTitle + R"rawliteral(</h1>
        
        <div class="page-badge )rawliteral"
                + String(idlePage ? "idle-badge" : "main-badge") + R"rawliteral(">
            )rawliteral"
                + String(idlePage ? "📋 IDLE CUES PAGE — shown when show is not running" : "🎬 MAIN SHOW PAGE — shown while the show is running") + R"rawliteral(
        </div>
        
        <div class="status-bar">
            <div class="status-item">
                <span class="status-label">Status:</span>
                <span class="status-value" id="stateDisplay">IDLE</span>
            </div>
            <div class="status-item">
                <span class="status-label">Elapsed:</span>
                <span class="status-value" id="timeDisplay">00:00:00</span>
            </div>
            <div class="status-item">
                <span class="status-label">IP:</span>
                <span class="status-ip" id="ipDisplay">)rawliteral"
                + ip + R"rawliteral(</span>
            </div>
        </div>
        
        <div class="controls">
            <button class="btn btn-start" id="startBtn" onclick="startShow()">▶ START SHOW</button>
            <button class="btn btn-reset" onclick="resetShow()">⏹ RESET SHOW</button>
            )rawliteral"
                + String(idlePage
                           ? "<button class=\"btn btn-idle\" onclick=\"window.location.href='/'\">🎬 GO TO MAIN SHOW</button>"
                           : "<button class=\"btn btn-idle\" onclick=\"window.location.href='/idle'\">📋 GO TO DEFAULT CUES</button>")
                + R"rawliteral(
        </div>
        
        <div class="config-section">
            <div class="config-group">
                <label>Product Title:</label>
                <input type="text" id="productTitle" value=")rawliteral"
                + safeTitle + R"rawliteral(">
            </div>
            <div class="config-group">
                <label>Show Duration (sec):</label>
                <input type="number" id="showDuration" value=")rawliteral"
                + String(config.showDuration) + R"rawliteral(" min="1" max="86400">
            </div>
            <div class="config-group">
                <label>Fade Duration (sec):</label>
                <input type="number" id="fadeDuration" value=")rawliteral"
                + String(config.fadeDuration) + R"rawliteral(" min="1" max="60">
            </div>
            <div class="config-group">
                <label>UDP Start Msg:</label>
                <input type="text" id="udpStartMsg" value=")rawliteral"
                + String(config.udpStartMsg) + R"rawliteral(" oninput="this.value=this.value.toLowerCase();" placeholder="startdmx">
            </div>
            <div class="config-group">
                <label>UDP Reset Msg:</label>
                <input type="text" id="udpStopMsg" value=")rawliteral"
                + String(config.udpStopMsg) + R"rawliteral(" oninput="this.value=this.value.toLowerCase();" placeholder="stopdmx">
            </div>
            <div class="config-group">
                <label>UDP Port:</label>
                <input type="number" id="udpPort" value=")rawliteral"
                + String(config.udpPort) + R"rawliteral(" min="1" max="65535" placeholder="10000">
            </div>
            <button class="btn btn-save" onclick="saveConfig()">💾 SAVE CONFIG</button>
        </div>
        
        <div class="table-container">
            <div style="margin-bottom:15px;display:flex;gap:10px;flex-wrap:wrap;">
                <button class="btn btn-delete" onclick="deleteSelected()">🗑 DELETE SELECTED</button>
                <button class="btn btn-save" onclick="saveCues()">💾 SAVE CUES</button>
                <button class="btn btn-idle" onclick="downloadBackup()">⬇ DOWNLOAD BACKUP</button>
                <button class="btn btn-idle" onclick="triggerUpload()">⬆ UPLOAD BACKUP</button>
                <input type="file" id="restoreFileInput" accept=".json,application/json" style="display:none" onchange="handleUploadFile(event)">
                <span style="flex:1;"></span>
                <span style="color:#888;">Page: <strong>)rawliteral"
                + String(idlePage ? "IDLE" : "MAIN") + R"rawliteral(</strong></span>
            </div>
            
            <table>
                <thead>
                    <tr>
                        <th style="width:40px;">#</th>
                        <th style="width:40px;">
                            <input type="checkbox" class="checkbox-input" id="selectAll" onchange="toggleAll()">
                        </th>
                        <th style="width:130px;">Time<br><small>(HH:MM:SS)</small></th>
                        <th style="width:85px;">Chan<br><small>(1-512)</small></th>
                        <th style="width:70px;">Fade In</th>
                        <th style="width:70px;">Fade Out</th>
                        <th style="width:95px;">Row/Start<br><small>(0-255)</small></th>
                        <th style="width:95px;">End<br><small>(0-255)</small></th>
                        <th style="width:50px;">Delete</th>
                    </tr>
                </thead>
                <tbody id="cueTableBody">
                </tbody>
            </table>
            
            <div class="table-footer">
                <div class="total-cues">Total: <span id="totalCues">0</span> cues</div>
                <div class="btn-group">
                    <button class="btn btn-add" onclick="addCue()">➕ ADD CUE</button>
                </div>
            </div>
        </div>
    </div>
    
    <script>
        let currentPage = ')rawliteral"
                + String(idlePage ? "idle" : "main") + R"rawliteral(';
        let cues = [];
        
        function loadCues() {
            fetch('/api/cues')
                .then(res => {
                    if (!res.ok) throw new Error('Network response was not ok');
                    return res.json();
                })
                .then(data => {
                    if (currentPage === 'idle') {
                        cues = data.idle || [];
                    } else {
                        cues = data.main || [];
                    }
                    renderTable();
                    updateStatus(data);
                })
                .catch(err => {
                    console.error('Error loading cues:', err);
                    setTimeout(loadCues, 3000);
                });
        }
        
        function renderTable() {
            const tbody = document.getElementById('cueTableBody');
            tbody.innerHTML = '';
            
            if (cues.length === 0) {
                tbody.innerHTML = '<tr><td colspan="9" style="text-align:center;color:#666;padding:20px;">No cues added yet. Click "ADD CUE" to get started!</td></tr>';
                document.getElementById('totalCues').textContent = '0';
                return;
            }
            
            cues.sort((a, b) => a.time - b.time);
            
            cues.forEach((cue, index) => {
                const row = document.createElement('tr');
                
                // Number
                const numCell = document.createElement('td');
                numCell.className = 'cue-number';
                numCell.textContent = index + 1;
                row.appendChild(numCell);
                
                // Checkbox
                const checkCell = document.createElement('td');
                const checkbox = document.createElement('input');
                checkbox.type = 'checkbox';
                checkbox.className = 'checkbox-input';
                checkbox.dataset.id = cue.id;
                checkCell.appendChild(checkbox);
                row.appendChild(checkCell);
                
                // Time
                const timeCell = document.createElement('td');
                const timeInput = document.createElement('input');
                timeInput.type = 'text';
                timeInput.className = 'time-input';
                timeInput.value = formatTime(cue.time);
                timeInput.dataset.id = cue.id;
                timeInput.dataset.field = 'time';
                timeInput.onchange = function() { updateCueField(this, 'time'); };
                timeCell.appendChild(timeInput);
                row.appendChild(timeCell);
                
                // Channel
                const chanCell = document.createElement('td');
                const chanInput = document.createElement('input');
                chanInput.type = 'number';
                chanInput.className = 'channel-input';
                chanInput.value = cue.channel;
                chanInput.min = 1;
                chanInput.max = 512;
                chanInput.dataset.id = cue.id;
                chanInput.dataset.field = 'channel';
                chanInput.onchange = function() { updateCueField(this, 'channel'); };
                chanCell.appendChild(chanInput);
                row.appendChild(chanCell);
                
                // Fade In
                const fadeInCell = document.createElement('td');
                const fadeInToggle = document.createElement('label');
                fadeInToggle.className = 'switch';
                const fadeInInput = document.createElement('input');
                fadeInInput.type = 'checkbox';
                fadeInInput.checked = cue.fadeIn;
                fadeInInput.dataset.id = cue.id;
                fadeInInput.dataset.field = 'fadeIn';
                fadeInInput.onchange = function() {
                  const row = this.closest('tr');
                    if (this.checked) {
                        const fadeOutInput = row.querySelector('input[data-field="fadeOut"]');
                        if (fadeOutInput) {
                            fadeOutInput.checked = false;
                            updateCueField(fadeOutInput, 'fadeOut');
                        }
                    }
                    updateCueField(this, 'fadeIn');
                    updateVisibility(row);
                };
                const slider = document.createElement('span');
                slider.className = 'slider';
                fadeInToggle.appendChild(fadeInInput);
                fadeInToggle.appendChild(slider);
                fadeInCell.appendChild(fadeInToggle);
                row.appendChild(fadeInCell);
                
                // Fade Out
                const fadeOutCell = document.createElement('td');
                const fadeOutToggle = document.createElement('label');
                fadeOutToggle.className = 'switch';
                const fadeOutInput = document.createElement('input');
                fadeOutInput.type = 'checkbox';
                fadeOutInput.checked = cue.fadeOut;
                fadeOutInput.dataset.id = cue.id;
                fadeOutInput.dataset.field = 'fadeOut';
                fadeOutInput.onchange = function() {
                  const row = this.closest('tr');
                    if (this.checked) {
                        const fadeInInput = row.querySelector('input[data-field="fadeIn"]');
                        if (fadeInInput) {
                            fadeInInput.checked = false;
                            updateCueField(fadeInInput, 'fadeIn');
                        }
                    }
                    updateCueField(this, 'fadeOut');
                    updateVisibility(row);
                };
                const sliderOut = document.createElement('span');
                sliderOut.className = 'slider';
                fadeOutToggle.appendChild(fadeOutInput);
                fadeOutToggle.appendChild(sliderOut);
                fadeOutCell.appendChild(fadeOutToggle);
                row.appendChild(fadeOutCell);
                
                // Row/Start Value
                const rowValCell = document.createElement('td');
                const rowValInput = document.createElement('input');
                rowValInput.type = 'number';
                rowValInput.className = 'value-input';
                if (cue.fadeIn || cue.fadeOut) {
                    rowValInput.value = cue.startValue;
                    rowValInput.dataset.field = 'startValue';
                } else {
                    rowValInput.value = cue.rowValue;
                    rowValInput.dataset.field = 'rowValue';
                }
                rowValInput.min = 0;
                rowValInput.max = 255;
                rowValInput.dataset.id = cue.id;
                rowValInput.onchange = function() { updateCueField(this, this.dataset.field); };
                rowValCell.appendChild(rowValInput);
                row.appendChild(rowValCell);
                
                // End Value
                const endValCell = document.createElement('td');
                const endValInput = document.createElement('input');
                endValInput.type = 'number';
                endValInput.className = 'value-input';
                if (currentPage === 'idle') {
                    endValInput.value = cue.fadeIn || cue.fadeOut ? cue.endValue : cue.rowValue;
                } else {
                    endValInput.value = cue.fadeIn || cue.fadeOut ? cue.endValue : '';
                }
                endValInput.min = 0;
                endValInput.max = 255;
                endValInput.dataset.id = cue.id;
                endValInput.dataset.field = 'endValue';
                endValInput.onchange = function() { updateCueField(this, 'endValue'); };
                endValCell.appendChild(endValInput);
                row.appendChild(endValCell);
                
                // Delete
                const delCell = document.createElement('td');
                const delBtn = document.createElement('button');
                delBtn.className = 'delete-btn';
                delBtn.textContent = '✕';
                delBtn.onclick = function() { deleteCue(cue.id); };
                delCell.appendChild(delBtn);
                row.appendChild(delCell);
                
                tbody.appendChild(row);
                updateVisibility(row);
            });
            
            document.getElementById('totalCues').textContent = cues.length;
        }
        
        function updateVisibility(row) {
            const fadeInCheck = row.querySelector('input[data-field="fadeIn"]');
            const fadeOutCheck = row.querySelector('input[data-field="fadeOut"]');
            const rowValCell = row.querySelector('td:nth-child(7)');
            const endValCell = row.querySelector('td:nth-child(8)');
            
            const fadeIn = fadeInCheck ? fadeInCheck.checked : false;
            const fadeOut = fadeOutCheck ? fadeOutCheck.checked : false;
            
            if (fadeIn || fadeOut) {
                rowValCell.querySelector('input').dataset.field = 'startValue';
                rowValCell.querySelector('input').placeholder = 'Start';
                endValCell.style.display = '';
                endValCell.querySelector('input').placeholder = 'End';
            } else {
                rowValCell.querySelector('input').dataset.field = 'rowValue';
                rowValCell.querySelector('input').placeholder = 'Row';
                if (currentPage === 'idle') {
                    endValCell.style.display = '';
                    endValCell.querySelector('input').placeholder = 'Value';
                    endValCell.querySelector('input').value = rowValCell.querySelector('input').value;
                } else {
                    endValCell.style.display = 'none';
                }
            }
        }
        
        function clamp(val, min, max) {
            if (isNaN(val)) return min;
            return Math.max(min, Math.min(max, val));
        }
        
        function updateCueField(element, field) {
            const id = parseInt(element.dataset.id);
            let value = element.type === 'checkbox' ? element.checked : parseInt(element.value);
            
            const cue = cues.find(c => c.id === id);
            if (cue) {
                if (field === 'time' && typeof element.value === 'string' && element.value.includes(':')) {
                    value = parseTime(element.value);
                }
                if (field === 'channel') {
                    value = clamp(value, 1, 512);
                    element.value = value;
                }
                if (field === 'rowValue' || field === 'startValue' || field === 'endValue') {
                    value = clamp(value, 0, 255);
                    element.value = value;
                }
                cue[field] = value;
                
                if (field === 'fadeIn' || field === 'fadeOut') {
                    const row = element.closest('tr');
                    updateVisibility(row);
                }
            }
        }
        
        function formatTime(seconds) {
            const h = String(Math.floor(seconds / 3600)).padStart(2, '0');
            const m = String(Math.floor((seconds % 3600) / 60)).padStart(2, '0');
            const s = String(seconds % 60).padStart(2, '0');
            return h + ':' + m + ':' + s;
        }
        
        function parseTime(str) {
            const parts = str.split(':');
            if (parts.length === 3) {
                return parseInt(parts[0]) * 3600 + parseInt(parts[1]) * 60 + parseInt(parts[2]);
            }
            return parseInt(str) || 0;
        }
        
        function addCue() {
            const newCue = {
                id: Date.now(),
                time: 0,
                channel: 1,
                fadeIn: false,
                fadeOut: false,
                rowValue: 0,
                startValue: 0,
                endValue: 255
            };
            cues.push(newCue);
            renderTable();
            saveCues(false);
        }
        
        function deleteCue(id) {
            if (confirm('Delete this cue?')) {
                cues = cues.filter(c => c.id !== id);
                renderTable();
                saveCues(false);
            }
        }
        
        function deleteSelected() {
            const checked = document.querySelectorAll('input[type="checkbox"][data-id]:checked');
            if (checked.length === 0) {
                alert('No cues selected!');
                return;
            }
            if (confirm('Delete ' + checked.length + ' selected cue(s)?')) {
                const ids = Array.from(checked).map(cb => parseInt(cb.dataset.id));
                cues = cues.filter(c => !ids.includes(c.id));
                renderTable();
                saveCues(false);
            }
        }
        
        function toggleAll() {
            const checked = document.getElementById('selectAll').checked;
            document.querySelectorAll('input[type="checkbox"][data-id]').forEach(cb => {
                cb.checked = checked;
            });
        }
        
        function saveCues(showAlert) {
            if (showAlert === undefined) showAlert = true;
            
            // First update all cues from inputs
            document.querySelectorAll('input[data-id]').forEach(input => {
                const id = parseInt(input.dataset.id);
                const field = input.dataset.field;
                let value = input.type === 'checkbox' ? input.checked : parseInt(input.value);
                const cue = cues.find(c => c.id === id);
                if (cue) {
                    if (field === 'time' && typeof input.value === 'string' && input.value.includes(':')) {
                        value = parseTime(input.value);
                    } else if (field === 'channel') {
                        value = clamp(value, 1, 512);
                    } else if (field === 'rowValue' || field === 'startValue' || field === 'endValue') {
                        value = clamp(value, 0, 255);
                    }
                    cue[field] = value;
                }
            });
            
            const form = new URLSearchParams();
            form.append('page', currentPage);
            form.append('cues', JSON.stringify(cues));

            fetch('/api/cues/save', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
              body: form.toString()
            })
            .then(res => res.json())
            .then(data => {
                if (data.success) {
                    console.log('Cues saved!');
                    if (showAlert) alert('Cues saved successfully!');
                } else {
                    alert('Error saving cues!');
                }
            })
            .catch(err => {
                console.error('Error saving cues:', err);
                alert('Error saving cues!');
            });
        }
        
        function saveConfig() {
            const productTitle = document.getElementById('productTitle').value;
            const showDuration = document.getElementById('showDuration').value;
            const fadeDuration = document.getElementById('fadeDuration').value;
            const udpStartMsg = document.getElementById('udpStartMsg').value.trim().toLowerCase();
            const udpStopMsg = document.getElementById('udpStopMsg').value.trim().toLowerCase();
            const udpPort = document.getElementById('udpPort').value;
            
            const form = new URLSearchParams();
            form.append('productTitle', productTitle);
            form.append('showDuration', showDuration);
            form.append('fadeDuration', fadeDuration);
            form.append('udpStartMsg', udpStartMsg);
            form.append('udpStopMsg', udpStopMsg);
            form.append('udpPort', udpPort);

            fetch('/api/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: form.toString()
            })
            .then(res => res.json())
            .then(data => {
                if (data.success) {
                    alert('Configuration saved successfully!');
                    // Reload page to show updated values
                    location.reload();
                } else {
                    alert('Error saving configuration!');
                }
            })
            .catch(err => {
                console.error('Error saving config:', err);
                alert('Error saving configuration!');
            });
        }
        
        function startShow() {
            if (confirm('Start the show?')) {
                fetch('/api/start', { method: 'POST' })
                    .then(res => res.json())
                    .then(data => {
                        if (data.success) {
                            loadCues();
                        } else {
                            alert('Error starting show: ' + (data.error || 'Unknown error'));
                        }
                    })
                    .catch(err => {
                        console.error('Error starting show:', err);
                        alert('Error starting show!');
                    });
            }
        }
        
        function resetShow() {
            if (confirm('Reset the show?')) {
                fetch('/api/reset', { method: 'POST' })
                    .then(res => res.json())
                    .then(data => {
                        if (data.success) {
                            loadCues();
                        } else {
                            alert('Error resetting show!');
                        }
                    })
                    .catch(err => {
                        console.error('Error resetting show:', err);
                        alert('Error resetting show!');
                    });
            }
        }
        
        function goToIdle() {
            window.location.href = '/idle';
        }
        
        // ---- Backup (download) / Restore (upload) for THIS page's cues ----
        // currentPage is 'idle' or 'main', so these always act on whichever
        // page (Idle Cues or Main Show) is currently open in the browser.
        
        function downloadBackup() {
            // Browser will prompt to save the file (server sets the filename
            // via Content-Disposition), separate for idle vs main.
            window.location.href = '/api/backup/' + currentPage;
        }
        
        function triggerUpload() {
            document.getElementById('restoreFileInput').click();
        }
        
        function handleUploadFile(event) {
            const file = event.target.files[0];
            event.target.value = ''; // allow re-selecting the same file later
            if (!file) return;
            
            const pageLabel = currentPage === 'idle' ? 'IDLE CUES' : 'MAIN SHOW';
            if (!confirm('This will REPLACE ALL current ' + pageLabel +
                         ' cues on the device with the contents of ' + file.name +
                         '. This cannot be undone. Continue?')) {
                return;
            }
            
            const reader = new FileReader();
            reader.onload = function(e) {
                fetch('/api/restore/' + currentPage, {
                    method: 'POST',
                    headers: { 'Content-Type': 'text/plain' },
                    body: e.target.result
                })
                .then(res => res.json())
                .then(data => {
                    if (data.success) {
                        alert('Restored ' + data.count + ' cues successfully!');
                        loadCues();
                    } else {
                        alert('Upload failed: ' + (data.error || 'Unknown error'));
                    }
                })
                .catch(err => {
                    console.error('Error uploading cues:', err);
                    alert('Upload failed - could not reach device.');
                });
            };
            reader.onerror = function() {
                alert('Could not read the selected file.');
            };
            reader.readAsText(file);
        }
        
        function updateStatus(data) {
            const stateDisplay = document.getElementById('stateDisplay');
            const timeDisplay = document.getElementById('timeDisplay');
            
            if (data.state === 'running') {
                stateDisplay.textContent = '▶ RUNNING';
                stateDisplay.className = 'status-value running';
                const elapsed = data.elapsedTime || 0;
                timeDisplay.textContent = formatTime(elapsed);
            } else {
                stateDisplay.textContent = '⏸ IDLE';
                stateDisplay.className = 'status-value idle';
                timeDisplay.textContent = '00:00:00';
            }
        }
        
        // Auto-refresh every second
        setInterval(() => {
            fetch('/api/cues')
                .then(res => res.json())
                .then(data => {
                    updateStatus(data);
                })
                .catch(err => {});
        }, 1000);
        
        // Load cues on page load
        loadCues();
    </script>
</body>
</html>
)rawliteral";
  return html;
}

// ===================== DMX ENGINE =====================

void updateDMX() {
  if (!dmxOK) return;

  if (currentState == STATE_IDLE) {
    processIdleCues();
  } else {
    processMainCues();
  }

  dmx_write(DMX_PORT, dmxData, DMX_PACKET_SIZE);
  dmx_send_num(DMX_PORT, DMX_PACKET_SIZE);
  dmx_wait_sent(DMX_PORT, DMX_TIMEOUT_TICK);
}

void processIdleCues() {
  for (auto& cue : idleCues) {
    if (!cue.fadeIn && !cue.fadeOut) {
      dmxData[cue.channel] = cue.rowValue;
    } else {
      dmxData[cue.channel] = cue.fadeIn ? cue.endValue : cue.endValue;
    }
  }
}

void processMainCues() {
  if (currentState != STATE_RUNNING) return;

  unsigned long currentTime = (millis() - showStartTime) / 1000;

  for (auto& active : activeFades) {
    if (active.completed) continue;

    float progress = (float)(millis() - active.startTime) / (active.duration * 1000);
    if (progress >= 1.0) {
      progress = 1.0;
      active.completed = true;
    }

    uint8_t value;
    if (active.fadeIn) {
      value = active.startValue + (active.endValue - active.startValue) * progress;
    } else {
      value = active.startValue - (active.startValue - active.endValue) * progress;
    }

    dmxData[active.channel] = value;
  }

  for (auto& cue : mainCues) {
    if (cue.active) continue;

    if (currentTime >= cue.time && currentTime < cue.time + config.fadeDuration) {
      cue.active = true;

      if (!cue.fadeIn && !cue.fadeOut) {
        dmxData[cue.channel] = cue.rowValue;
      } else {
        ActiveCue active;
        active.cueId = cue.id;
        active.channel = cue.channel;
        active.fadeIn = cue.fadeIn;
        active.fadeOut = cue.fadeOut;
        active.startValue = cue.startValue;
        active.endValue = cue.endValue;
        active.startTime = millis();
        active.duration = config.fadeDuration;
        active.completed = false;
        activeFades.push_back(active);
        dmxData[cue.channel] = cue.startValue;
      }
    }
  }

  activeFades.erase(
    std::remove_if(activeFades.begin(), activeFades.end(),
                   [](const ActiveCue& a) {
                     return a.completed;
                   }),
    activeFades.end());
}

void resetShow() {
  currentState = STATE_IDLE;
  elapsedSeconds = 0;
  activeFades.clear();
  memset(dmxData, 0, DMX_PACKET_SIZE);
  processIdleCues();

  for (auto& cue : mainCues) {
    cue.active = false;
  }

  Serial.println("Show reset to idle");
}

// ===================== OLED =====================

void updateOLED() {
  if (!oledOK) return;

  display.clearDisplay();
  display.drawRect(0, 0, 128, 64, SH110X_WHITE);
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(5, 4);
  display.println("GUARDIAN MEDIA PUNE");
  display.setCursor(5, 15);
  display.println(config.productTitle);

  if (currentState == STATE_RUNNING) {
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(5, 40);
    display.println("SHOW STARTED");
  } else {
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(5, 40);
    display.println("WAITING FOR TRIGGER");
  }

  String ip = wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(5, 28);
  display.println("IP: " + ip);

  if (currentState == STATE_RUNNING) {
    unsigned long elapsed = (millis() - showStartTime) / 1000;
    char timeStr[9];
    sprintf(timeStr, "%02lu:%02lu:%02lu",
            (elapsed / 3600) % 24,
            (elapsed / 60) % 60,
            elapsed % 60);
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(5, 52);
    display.println(timeStr);
  }

  display.display();
}

// ===================== STORAGE =====================

void saveDataToSPIFFS() {
  Serial.println("Saving data to SPIFFS...");

  DynamicJsonDocument doc(200000);

  JsonObject configObj = doc.createNestedObject("config");
  configObj["productTitle"] = config.productTitle;
  configObj["showDuration"] = config.showDuration;
  configObj["fadeDuration"] = config.fadeDuration;
  configObj["udpStartMsg"] = config.udpStartMsg;
  configObj["udpStopMsg"] = config.udpStopMsg;
  configObj["udpPort"] = config.udpPort;

  JsonArray idleArray = doc.createNestedArray("idleCues");
  for (const auto& cue : idleCues) {
    JsonObject obj = idleArray.createNestedObject();
    obj["id"] = cue.id;
    obj["time"] = cue.time;
    obj["channel"] = cue.channel;
    obj["fadeIn"] = cue.fadeIn;
    obj["fadeOut"] = cue.fadeOut;
    obj["rowValue"] = cue.rowValue;
    obj["startValue"] = cue.startValue;
    obj["endValue"] = cue.endValue;
  }

  JsonArray mainArray = doc.createNestedArray("mainCues");
  for (const auto& cue : mainCues) {
    JsonObject obj = mainArray.createNestedObject();
    obj["id"] = cue.id;
    obj["time"] = cue.time;
    obj["channel"] = cue.channel;
    obj["fadeIn"] = cue.fadeIn;
    obj["fadeOut"] = cue.fadeOut;
    obj["rowValue"] = cue.rowValue;
    obj["startValue"] = cue.startValue;
    obj["endValue"] = cue.endValue;
  }

  File file = SPIFFS.open("/data.json", "w");
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }

  serializeJson(doc, file);
  file.close();
  Serial.println("Data saved successfully");
}

void loadDataFromSPIFFS() {
  Serial.println("Loading data from SPIFFS...");

  if (!SPIFFS.exists("/data.json")) {
    Serial.println("No saved data found, using defaults");
    Cue defaultIdle;
    defaultIdle.id = nextCueId++;
    defaultIdle.time = 0;
    defaultIdle.channel = 1;
    defaultIdle.fadeIn = false;
    defaultIdle.fadeOut = false;
    defaultIdle.rowValue = 0;
    defaultIdle.startValue = 0;
    defaultIdle.endValue = 255;
    idleCues.push_back(defaultIdle);
    return;
  }

  File file = SPIFFS.open("/data.json", "r");
  if (!file) {
    Serial.println("Failed to open file for reading");
    return;
  }

  DynamicJsonDocument doc(200000);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.println("Failed to parse JSON data");
    return;
  }

  if (doc.containsKey("config")) {
    JsonObject configObj = doc["config"];
    const char* savedTitle = configObj["productTitle"] | "DMX CONTROLLER";
    strncpy(config.productTitle, savedTitle, sizeof(config.productTitle) - 1);
    config.productTitle[sizeof(config.productTitle) - 1] = '\0';
    config.showDuration = configObj["showDuration"] | 300;
    config.fadeDuration = configObj["fadeDuration"] | 2;
    if (config.showDuration < 1 || config.showDuration > 86400) config.showDuration = 300;
    if (config.fadeDuration < 1 || config.fadeDuration > 60) config.fadeDuration = 2;

    const char* savedUdpStart = configObj["udpStartMsg"] | "startdmx";
    strncpy(config.udpStartMsg, savedUdpStart, sizeof(config.udpStartMsg) - 1);
    config.udpStartMsg[sizeof(config.udpStartMsg) - 1] = '\0';
    for (char* p = config.udpStartMsg; *p; p++) *p = tolower(*p);

    const char* savedUdpStop = configObj["udpStopMsg"] | "stopdmx";
    strncpy(config.udpStopMsg, savedUdpStop, sizeof(config.udpStopMsg) - 1);
    config.udpStopMsg[sizeof(config.udpStopMsg) - 1] = '\0';
    for (char* p = config.udpStopMsg; *p; p++) *p = tolower(*p);

    config.udpPort = configObj["udpPort"] | 10000;
    if (config.udpPort < 1) config.udpPort = 10000;
  }

  if (doc.containsKey("idleCues")) {
    JsonArray idleArray = doc["idleCues"];
    for (JsonObject obj : idleArray) {
      Cue cue;
      cue.id = obj["id"] | nextCueId;
      if (cue.id >= nextCueId) nextCueId = cue.id + 1;
      cue.time = obj["time"] | 0;
      cue.channel = clampLong(obj["channel"] | 1, 1, 512);
      cue.fadeIn = obj["fadeIn"] | false;
      cue.fadeOut = obj["fadeOut"] | false;
      cue.rowValue = clampLong(obj["rowValue"] | 0, 0, 255);
      cue.startValue = clampLong(obj["startValue"] | 0, 0, 255);
      cue.endValue = clampLong(obj["endValue"] | 255, 0, 255);
      cue.active = false;
      idleCues.push_back(cue);
    }
  }

  if (doc.containsKey("mainCues")) {
    JsonArray mainArray = doc["mainCues"];
    for (JsonObject obj : mainArray) {
      Cue cue;
      cue.id = obj["id"] | nextCueId;
      if (cue.id >= nextCueId) nextCueId = cue.id + 1;
      cue.time = obj["time"] | 0;
      cue.channel = clampLong(obj["channel"] | 1, 1, 512);
      cue.fadeIn = obj["fadeIn"] | false;
      cue.fadeOut = obj["fadeOut"] | false;
      cue.rowValue = clampLong(obj["rowValue"] | 0, 0, 255);
      cue.startValue = clampLong(obj["startValue"] | 0, 0, 255);
      cue.endValue = clampLong(obj["endValue"] | 255, 0, 255);
      cue.active = false;
      mainCues.push_back(cue);
    }
  }

  Serial.printf("Loaded %d idle cues, %d main cues\n", idleCues.size(), mainCues.size());
}

// ===================== BUTTONS =====================

void checkButtons() {
  if (digitalRead(BTN_START) == HIGH && millis() - lastStartPress > 500) {
    lastStartPress = millis();
    if (startShowAction()) {
      Serial.println("Show started via GPIO");
    }
  }

  if (digitalRead(BTN_STOP) == HIGH && millis() - lastStopPress > 500) {
    lastStopPress = millis();
    if (currentState == STATE_RUNNING) {
      resetShow();
      Serial.println("Show reset via GPIO");
    }
  }
}