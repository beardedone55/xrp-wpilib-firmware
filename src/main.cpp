#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SingleFileDrive.h>
#include <WebServer.h>
#include <WebSockets4WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <Wire.h>

#include <vector>

#include "config.h"
#include "imu.h"
#include "robot.h"
#include "wpilibws.h"

// Resource strings
extern "C" {
const unsigned char* GetResource_index_html(size_t* len);
const unsigned char* GetResource_normalize_css(size_t* len);
const unsigned char* GetResource_skeleton_css(size_t* len);
const unsigned char* GetResource_xrp_js(size_t* len);
}

char chipID[20];
char DEFAULT_SSID[32];

XRPConfiguration config;
NetworkMode netConfigResult;

// HTTP and WS Servers
WebServer webServer(5000);
WebSocketsServer wsServer(3300);

std::vector<std::string> outboundMessages;

// TEMP: Status
unsigned long _wsMessageCount = 0;
unsigned long _lastMessageStatusPrint = 0;
int _baselineUsedHeap = 0;

unsigned long _avgLoopTimeUs = 0;
unsigned long _loopTimeMeasurementCount = 0;

// Generate the status text file
void writeStatusToDisk() {
  File f = LittleFS.open("/status.txt", "w");
  f.printf("Chip ID: %s\n", chipID);
  f.printf("WiFi Mode: %s\n", netConfigResult == NetworkMode::AP ? "AP" : "STA");
  if (netConfigResult == NetworkMode::AP) {
    f.printf("AP SSID: %s\n", config.networkConfig.defaultAPName.c_str());
    f.printf("AP PASS: %s\n", config.networkConfig.defaultAPPassword.c_str());
  }
  else {
    f.printf("Connected to %s\n", WiFi.SSID().c_str());
  }
 
  f.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
  f.close();
}

// void handleIndexRoute() {
//   webServer.send(200, "text/plain", "You probably want the WS interface on /wpilibws");
// }

// ==================================================
// Web Server Management Functions
// ==================================================
void setupWebServerRoutes() {
  webServer.on("/", []() {
    size_t len;
    webServer.send(200, "text/html", GetResource_index_html(&len), len);
  });

  webServer.on("/normalize.css", []() {
    size_t len;
    webServer.send(200, "text/css", GetResource_normalize_css(&len), len);
  });

  webServer.on("/skeleton.css", []() {
    size_t len;
    webServer.send(200, "text/css", GetResource_skeleton_css(&len), len);
  });

  webServer.on("/xrp.js", []() {
    size_t len;
    webServer.send(200, "text/javascript", GetResource_xrp_js(&len), len);
  });

  webServer.on("/getconfig", []() {
    File f = LittleFS.open("/config.json", "r");
    if (webServer.streamFile(f, "text/json") != f.size()) {
      Serial.println("[WEB] Sent less data than expected for /getconfig");
    }
    f.close();
  });

  webServer.on("/resetconfig", []() {
    if (webServer.method() != HTTP_POST) {
      webServer.send(405, "text/plain", "Method Not Allowed");
      return;
    }
    File f = LittleFS.open("/config.json", "w");
    f.print(generateDefaultConfig(DEFAULT_SSID).toJsonString().c_str());
    f.close();
    webServer.send(200, "text/plain", "OK");
  });

  webServer.on("/saveconfig", []() {
    if (webServer.method() != HTTP_POST) {
      webServer.send(405, "text/plain", "Method Not Allowed");
      return;
    }
    auto postBody = webServer.arg("plain");
    File f = LittleFS.open("/config.json", "w");
    f.print(postBody);
    f.close();
    Serial.println("[CONFIG] Configuration Updated Remotely");

    webServer.send(200, "text/plain", "OK");
  });
}

void sendMessage(std::string msg) {
  outboundMessages.push_back(msg);
}

void checkAndSendMessages() {
  for (auto msg : outboundMessages) {
    wsServer.broadcastTXT(msg.c_str());
  }

  outboundMessages.clear();
}

void handleWSEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[NET:WS] [%u] Disconnected\n", num);
      break;
    case WStype_CONNECTED: {
        IPAddress ip = wsServer.remoteIP(num);
        Serial.printf("[NET:WS] [%u] Connection from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
      }
      break;
    case WStype_TEXT: {
        StaticJsonDocument<512> jsonDoc;
        DeserializationError error = deserializeJson(jsonDoc, payload);
        if (error) {
          Serial.println(error.f_str());
          break;
        }

        _wsMessageCount++;
        wpilibws::processWSMessage(jsonDoc);
      }
      break;
  }
}

void checkPrintStatus() {
  if (millis() - _lastMessageStatusPrint > 5000) {
    int numConnectedClients = wsServer.connectedClients();
    int usedHeap = rp2040.getUsedHeap();
    Serial.printf("t(ms):%u c:%d h:%d msg:%u lt(us):%u\n", millis(), numConnectedClients, usedHeap, _wsMessageCount, _avgLoopTimeUs);
    _lastMessageStatusPrint = millis();
  }
}

void updateLoopTime(unsigned long loopStart) {
  unsigned long loopTime = micros() - loopStart;
  unsigned long totalTime = _avgLoopTimeUs * _loopTimeMeasurementCount;
  _loopTimeMeasurementCount++;

  _avgLoopTimeUs = (totalTime + loopTime) / _loopTimeMeasurementCount;
}


void setup() {
  // Generate the default SSID using the flash ID
  pico_unique_board_id_t id_out;
  pico_get_unique_board_id(&id_out);
  sprintf(chipID, "%02x%02x-%02x%02x", id_out.id[4], id_out.id[5], id_out.id[6], id_out.id[7]);
  sprintf(DEFAULT_SSID, "XRP-%s", chipID);

  Serial.begin(115200);
  LittleFS.begin();

  // Set up the I2C pins
  Wire1.setSCL(19);
  Wire1.setSDA(18);
  Wire1.begin();

  delay(2000);

  // Read Config
  config = loadConfiguration(DEFAULT_SSID);

  // Initialize IMU
  Serial.println("[IMU] Initializing IMU");
  xrp::imuInit(IMU_I2C_ADDR, &Wire1);

  Serial.println("[IMU] Beginning IMU calibration");
  xrp::imuCalibrate(5000);

  // Busy-loop if there's no WiFi hardware
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("[NET] No WiFi Module");
    while (true);
  }

  // Set up WiFi AP
  WiFi.setHostname("XRP");

  // Use configuration information
  netConfigResult = configureNetwork(config);
  Serial.printf("[NET] Actual WiFi Mode: %s\n", netConfigResult == NetworkMode::AP ? "AP" : "STA");

  // bool result = WiFi.softAP(DEFAULT_SSID, "xrp-wpilib");
  // if (result) {
  //   Serial.println("[NET] WiFi AP Ready");
  // }
  // else {
  //   Serial.println("[NET] AP Set up failed");
  //   while (true);
  // }

  // Set up HTTP server routes
  Serial.println("[NET] Setting up Config webserver");
  setupWebServerRoutes();

  webServer.begin();
  Serial.println("[NET] Config webserver listening on *:5000");

  Serial.println("[NET] Setting up WS Server");
  wsServer.onEvent(handleWSEvent);
  wsServer.begin();

  Serial.println("[NET] Network Ready");
  Serial.printf("[NET] SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("[NET] IP: %s\n", WiFi.localIP().toString().c_str());

  xrp::robotInit();

  _lastMessageStatusPrint = millis();
  _baselineUsedHeap = rp2040.getUsedHeap();

  // Write current status file
  writeStatusToDisk();
  singleFileDrive.begin("status.txt", "XRP-Status.txt");
}

int lastCheckedNumClients = 0;
void loop() {
  unsigned long loopStartTime = micros();

  webServer.handleClient();
  wsServer.loop();

  // TODO always run the IMU periodic routine
  xrp::imuPeriodic();

  // Disable the robot when we no longer have a connection
  int numConnectedClients = wsServer.connectedClients();
  if (lastCheckedNumClients > 0 && numConnectedClients == 0) {
    xrp::robotSetEnabled(false);
    xrp::imuSetEnabled(false);
    outboundMessages.clear();
  }
  lastCheckedNumClients = numConnectedClients;

  if (numConnectedClients > 0) {
    // Send any messages we need to
    checkAndSendMessages();

    // Read sensor data
    auto updatedData = xrp::robotPeriodic();
    if (updatedData & XRP_DATA_ENCODER) {
      auto encValues = xrp::getActiveEncoderValues();
      for (auto encData : encValues) {
        sendMessage(wpilibws::makeEncoderMessage(encData.first, encData.second));
      }
    }

    if (updatedData & XRP_DATA_DIO) {
      // User button is on DIO 0
      sendMessage(wpilibws::makeDIOMessage(0, xrp::isUserButtonPressed()));
    }

    // Read Gyro Data
    if (xrp::imuDataReady()) {
      float rateZ = xrp::imuGetGyroRateZ();
      float yawAngle = xrp::imuGetYaw();
      
      sendMessage(wpilibws::makeGyroSingleMessage(wpilibws::AXIS_Z, rateZ, yawAngle));
    }

  }

  updateLoopTime(loopStartTime);
  checkPrintStatus();
}
