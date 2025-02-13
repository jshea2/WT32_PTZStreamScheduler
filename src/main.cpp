#include <ETH.h>
#include <AsyncTCP.h>                    // Required for ESP32
#include <ESPAsyncWebServer.h>           // Asynchronous HTTP and WebSocket Server
#include <TimeLib.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <HTTPClient.h>
#include <vector>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Timezone.h>

// ----- Global Status Variables -----
// Updated by the PTZ control task.
String globalCurrentTime = "";
String globalCurrentDate = "";
int    globalRtmpStatus = -1;        // -1 indicates error/unavailable
bool   globalInternetConnected = false;
bool   globalNtpUpdated = false;
bool   globalDesiredStream = false;  // True if an event window is active

// ----- Ethernet & Hardware Definitions -----
#define ETH_ADDR        1
#define ETH_POWER_PIN   16
#define ETH_MDC_PIN     23
#define ETH_MDIO_PIN    18
#define ETH_TYPE        ETH_PHY_LAN8720
#define ETH_CLK_MODE    ETH_CLOCK_GPIO17_OUT

IPAddress local_IP(10, 0, 3, 3);
IPAddress gateway(10, 0, 3, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns1(8, 8, 8, 8);
IPAddress dns2(8, 8, 4, 4);

// ----- Global Objects -----
AsyncWebServer server(80);
WiFiUDP ntpUDP;
// 60-second update interval
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

// ----- Timezone Definitions -----
TimeChangeRule usPDT = {"PDT", Second, Sun, Mar, 2, -420}; // PDT = UTC - 7 hours
TimeChangeRule usPST = {"PST", First, Sun, Nov, 2, -480};   // PST = UTC - 8 hours
Timezone usPacific(usPDT, usPST);

// ----- Application Variables -----
struct Event {
  String date;
  String startTime;
  String stopTime;
};

String ptzCameraIP = "10.0.3.61";
// No longer using separate flags for start/stop commands.
std::vector<Event> events;
bool daylightSavingTime = false; // Stored in settings

// ----- Utility Functions -----

// Returns formatted time "HH:MM"
String getFormattedTime(time_t rawTime) {
  char timeStr[6];
  sprintf(timeStr, "%02d:%02d", hour(rawTime), minute(rawTime));
  return String(timeStr);
}

// Returns formatted date "YYYY-MM-DD"
String getFormattedDate(time_t rawTime) {
  char dateStr[11];
  sprintf(dateStr, "%04d-%02d-%02d", year(rawTime), month(rawTime), day(rawTime));
  return String(dateStr);
}

// Synchronous HTTP GET with a short timeout and minimal retries.
void triggerHttpGetWithRetry(String ipAddress, String command, int retries = 1) {
  int attempt = 0;
  bool success = false;
  while (attempt < retries && !success) {
    HTTPClient http;
    String url = "http://" + ipAddress + command;
    http.setTimeout(1000); // 1-second timeout
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode > 0) {
      Serial.println("HTTP GET request sent successfully: " + url);
      success = true;
    } else {
      Serial.println("HTTP GET request failed for: " + url + " (attempt " + String(attempt) + ")");
      vTaskDelay(100 / portTICK_PERIOD_MS);
      attempt++;
    }
    http.end();
  }
}

// Synchronous HTTP call to get RTMP status.
int getRTMPStatus(String ipAddress) {
  HTTPClient http;
  String url = "http://" + ipAddress + "/cgi-bin/get_rtmp_status";
  http.setTimeout(1000);
  http.begin(url);
  int httpCode = http.GET();
  int rtmpStatus = -1;
  if (httpCode > 0) {
    String payload = http.getString();
    int statusIndex = payload.indexOf("status=");
    if (statusIndex != -1) {
      rtmpStatus = payload.substring(statusIndex + 7).toInt();
    }
  }
  http.end();
  return rtmpStatus;
}

// Synchronous HTTP call to check Internet connectivity.
bool checkInternetConnectivity() {
  HTTPClient http;
  http.setTimeout(1000);
  http.begin("http://clients3.google.com/generate_204");
  int httpCode = http.GET();
  http.end();
  return (httpCode == 204);
}

// Connect and configure Ethernet (blocking until connected)
void connectEthernet() {
  unsigned long startTime = millis();
  const unsigned long timeout = 60000;
  while (true) {
    if (ETH.linkUp() && ETH.localIP() != IPAddress(0, 0, 0, 0)) {
      Serial.println("\nEthernet connected");
      Serial.print("IP Address: ");
      Serial.println(ETH.localIP());
      Serial.print("Gateway: ");
      Serial.println(gateway);
      Serial.print("Subnet: ");
      Serial.println(subnet);
      Serial.print("DNS 1: ");
      Serial.println(dns1);
      Serial.print("DNS 2: ");
      Serial.println(dns2);
      break;
    } else {
      Serial.println("Ethernet not connected. Retrying...");
    }
    if (millis() - startTime > timeout) {
      Serial.println("Ethernet not connected after timeout. Rebooting...");
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      ESP.restart();
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// Save settings and events to SPIFFS.
void saveSettings() {
  StaticJsonDocument<2048> json;
  json["ptzCameraIP"] = ptzCameraIP;
  json["utcOffsetInSeconds"] = (long)0; // Unused here.
  json["daylightSavingTime"] = daylightSavingTime;
  JsonArray eventsArray = json.createNestedArray("events");
  for (const auto &event : events) {
    JsonObject eventObj = eventsArray.createNestedObject();
    eventObj["date"] = event.date;
    eventObj["startTime"] = event.startTime;
    eventObj["stopTime"] = event.stopTime;
  }
  File file = SPIFFS.open("/settings.json", FILE_WRITE);
  if (file) {
    serializeJson(json, file);
    file.close();
    Serial.println("Settings saved to SPIFFS.");
  } else {
    Serial.println("Failed to open file for writing.");
  }
}

// Load settings and events from SPIFFS.
void loadSettings() {
  File file = SPIFFS.open("/settings.json", FILE_READ);
  if (file) {
    StaticJsonDocument<2048> json;
    DeserializationError error = deserializeJson(json, file);
    if (!error) {
      ptzCameraIP = json["ptzCameraIP"].as<String>();
      daylightSavingTime = json["daylightSavingTime"];
      events.clear();
      JsonArray eventsArray = json["events"];
      for (JsonObject eventObj : eventsArray) {
        Event event;
        event.date = eventObj["date"].as<String>();
        event.startTime = eventObj["startTime"].as<String>();
        event.stopTime = eventObj["stopTime"].as<String>();
        events.push_back(event);
      }
      Serial.println("Settings loaded from SPIFFS.");
    } else {
      Serial.println("Failed to deserialize JSON.");
    }
    file.close();
  } else {
    Serial.println("Failed to open file for reading.");
  }
}

// ----- PTZ Control Task -----
// Runs on core 1: Checks events, RTMP status, and updates global status variables.
void ptzControlTask(void * parameter) {
  static unsigned long lastEventCheckTime = 0;
  static unsigned long lastRTMPCheckTime = 0;
  for (;;) {
    unsigned long currentMillis = millis();
    
    // Update global time and connectivity status.
    time_t rawTime = timeClient.getEpochTime();
    time_t localTime = usPacific.toLocal(rawTime);
    globalCurrentTime = getFormattedTime(localTime);
    globalCurrentDate = getFormattedDate(localTime);
    globalInternetConnected = checkInternetConnectivity();
    // Assume NTP was updated in setup.
    globalNtpUpdated = true;
    
    // Event Check (every 60 seconds):
    if (currentMillis - lastEventCheckTime >= 60000) {
      int currentTotalMinutes = hour(localTime) * 60 + minute(localTime);
      bool desired = false;
      for (const auto &event : events) {
        if (globalCurrentDate == event.date) {
          int startHour = event.startTime.substring(0, 2).toInt();
          int startMinute = event.startTime.substring(3, 5).toInt();
          int stopHour = event.stopTime.substring(0, 2).toInt();
          int stopMinute = event.stopTime.substring(3, 5).toInt();
          int startTotal = startHour * 60 + startMinute;
          int stopTotal = stopHour * 60 + stopMinute;
          if (currentTotalMinutes >= startTotal && currentTotalMinutes < stopTotal) {
            desired = true;
            break;
          }
        }
      }
      globalDesiredStream = desired;
      lastEventCheckTime = currentMillis;
    }
    
    // RTMP Status Check (every 4 seconds):
    if (currentMillis - lastRTMPCheckTime >= 4000) {
      globalRtmpStatus = getRTMPStatus(ptzCameraIP);
      if (globalDesiredStream && globalRtmpStatus != 1) {
        triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/rtmp_ctrl?cmd=start");
      } else if (!globalDesiredStream && globalRtmpStatus != 0) {
        triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/rtmp_ctrl?cmd=stop");
      }
      lastRTMPCheckTime = currentMillis;
    }
    
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ----- Setup -----
void setup() {
  pinMode(ETH_POWER_PIN, OUTPUT);
  digitalWrite(ETH_POWER_PIN, HIGH);
  
  Serial.begin(115200);
  delay(5000); // Allow time for initialization
  Serial.println("Starting...");
  
  if (!SPIFFS.begin(true)) {
    Serial.println("Error mounting SPIFFS");
  }
  loadSettings();
  
  ETH.begin(ETH_ADDR, ETH_POWER_PIN, ETH_MDC_PIN, ETH_MDIO_PIN, ETH_TYPE, ETH_CLK_MODE);
  if (!ETH.config(local_IP, gateway, subnet, dns1, dns2)) {
    Serial.println("Ethernet configuration failed.");
  } else {
    Serial.println("Ethernet configured successfully.");
  }
  connectEthernet();
  
  timeClient.begin();
  if (timeClient.forceUpdate()) {
    globalNtpUpdated = true;
    Serial.println("NTP time updated successfully in setup.");
  } else {
    Serial.println("NTP time update failed in setup.");
  }
  
  // ----- Async Web Server Routes -----
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String htmlPage = "<html><head><meta charset='UTF-8'><title>PTZ Stream Scheduler</title></head><body>";
    htmlPage += "<h1>PTZ Stream Scheduler</h1>";
    htmlPage += "<p>Internet Connected: <span id='internetConnected'>Loading...</span></p>";
    htmlPage += "<p>NTP Updated: <span id='ntpUpdated'>Loading...</span></p>";
    htmlPage += "<p>Current Date: <span id='currentDate'>Loading...</span></p>";
    htmlPage += "<p>Current Time: <span id='currentTime'>Loading...</span></p>";
    htmlPage += "<form action='/updateSettings' method='post' id='settingsForm'>";
    for (size_t i = 0; i < events.size(); ++i) {
      htmlPage += "<div id='event" + String(i) + "'>";
      htmlPage += "<h2>Event " + String(i+1) + "</h2>";
      htmlPage += "<label for='startDate" + String(i) + "'>Start Date (YYYY-MM-DD):</label><br>";
      htmlPage += "<input type='text' id='startDate" + String(i) + "' name='startDate" + String(i) + "' value='" + events[i].date + "'><br><br>";
      htmlPage += "<label for='startTime" + String(i) + "'>Start Time (HH:MM):</label><br>";
      htmlPage += "<input type='text' id='startTime" + String(i) + "' name='startTime" + String(i) + "' value='" + events[i].startTime + "'><br><br>";
      htmlPage += "<label for='stopTime" + String(i) + "'>Stop Time (HH:MM):</label><br>";
      htmlPage += "<input type='text' id='stopTime" + String(i) + "' name='stopTime" + String(i) + "' value='" + events[i].stopTime + "'><br><br>";
      htmlPage += "<button type='button' onclick='deleteEvent(" + String(i) + ")'>Delete Event</button><br><br>";
      htmlPage += "</div>";
    }
    htmlPage += "<button type='button' onclick='addEvent()'>Add Event</button><br><br>";
    htmlPage += "<label for='ip'>PTZ Camera IP:</label><br>";
    htmlPage += "<input type='text' id='ip' name='ip' value='" + ptzCameraIP + "'><br><br>";
    htmlPage += "<input type='submit' value='Update Settings'>";
    htmlPage += "</form>";
    htmlPage += "<h2>Current Settings</h2>";
    for (size_t i = 0; i < events.size(); ++i) {
      htmlPage += "<p>Event " + String(i+1) + ":</p>";
      htmlPage += "<p>Start Date: " + events[i].date + "</p>";
      htmlPage += "<p>Start Time: " + events[i].startTime + "</p>";
      htmlPage += "<p>Stop Time: " + events[i].stopTime + "</p>";
    }
    htmlPage += "<p>PTZ Camera IP: " + ptzCameraIP + "</p>";
    htmlPage += "<p>PTZ: <span id='ptzStatus'>Loading...</span></p>";
    htmlPage += "<p>Stream Status: <span id='streamStatus'>Loading...</span></p>";
    htmlPage += "<script>";
    htmlPage += "function addEvent() { var form = document.getElementById('settingsForm'); form.action = '/addEvent'; form.submit(); }";
    htmlPage += "function deleteEvent(index) { var form = document.getElementById('settingsForm'); form.action = '/deleteEvent?index=' + index; form.submit(); }";
    htmlPage += "function triggerPreset1() { var xhttp = new XMLHttpRequest(); xhttp.open('GET', '/preset1Action', true); xhttp.send(); }";
    htmlPage += "function triggerPreset2() { var xhttp = new XMLHttpRequest(); xhttp.open('GET', '/preset2Action', true); xhttp.send(); }";
    htmlPage += "function triggerPreset3() { var xhttp = new XMLHttpRequest(); xhttp.open('GET', '/preset3Action', true); xhttp.send(); }";
    htmlPage += "function triggerPowerOn() { var xhttp = new XMLHttpRequest(); xhttp.open('GET', '/powerOnAction', true); xhttp.send(); }";
    htmlPage += "function triggerDeskMode() { var xhttp = new XMLHttpRequest(); xhttp.open('GET', '/deskModeAction', true); xhttp.send(); }";
    htmlPage += "function updateStatus() {";
    htmlPage += "  var xhr = new XMLHttpRequest();";
    htmlPage += "  xhr.open('GET', '/status', true);";
    htmlPage += "  xhr.onload = function() {";
    htmlPage += "    if (this.status == 200) {";
    htmlPage += "      var data = JSON.parse(this.responseText);";
    htmlPage += "      document.getElementById('currentTime').innerHTML = data.currentTime;";
    htmlPage += "      document.getElementById('currentDate').innerHTML = data.currentDate;";
    htmlPage += "      document.getElementById('internetConnected').innerHTML = data.internetConnected ? 'Yes' : 'No';";
    htmlPage += "      document.getElementById('ntpUpdated').innerHTML = data.ntpUpdated ? 'Yes' : 'No';";
    htmlPage += "      document.getElementById('ptzStatus').innerHTML = data.ptzStatus;";
    htmlPage += "      document.getElementById('streamStatus').innerHTML = data.streamStatus;";
    htmlPage += "    }";
    htmlPage += "  };";
    htmlPage += "  xhr.send();";
    htmlPage += "}";
    htmlPage += "setInterval(updateStatus, 1000);";
    htmlPage += "</script>";
    htmlPage += "<h3>PTZ Controls</h3>";
    htmlPage += "<button onclick='triggerPowerOn()'>Power ON</button>&nbsp;";
    htmlPage += "<button onclick='triggerPreset1()'>Preset 1</button>&nbsp;";
    htmlPage += "<button onclick='triggerPreset2()'>Preset 2</button>&nbsp;";
    htmlPage += "<button onclick='triggerPreset3()'>Preset 3</button>&nbsp;";
    htmlPage += "<button onclick='triggerDeskMode()'>Desk Mode</button>";
    htmlPage += "</body></html>";
    request->send(200, "text/html", htmlPage);
  });
  
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["internetConnected"] = globalInternetConnected;
    doc["ntpUpdated"] = globalNtpUpdated;
    doc["currentTime"] = globalCurrentTime;
    doc["currentDate"] = globalCurrentDate;
    doc["ptzStatus"] = (globalRtmpStatus != -1) ? "Connected" : "Not Connected";
    doc["streamStatus"] = (globalRtmpStatus == 1) ? "During Stream" : "Stream Suspended";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });
  
  server.on("/addEvent", HTTP_POST, [](AsyncWebServerRequest *request) {
    Event newEvent = {"YYYY-MM-DD", "HH:MM", "HH:MM"};
    events.push_back(newEvent);
    saveSettings();
    request->redirect("/");
  });
  
  server.on("/deleteEvent", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("index", true)) {
      int index = request->getParam("index", true)->value().toInt();
      if (index >= 0 && index < events.size()) {
        events.erase(events.begin() + index);
        saveSettings();
      }
    }
    request->redirect("/");
  });
  
  server.on("/preset1Action", HTTP_GET, [](AsyncWebServerRequest *request) {
    triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23R00&res=1");
    request->send(200, "text/plain", "Preset 1 Triggered");
  });
  server.on("/preset2Action", HTTP_GET, [](AsyncWebServerRequest *request) {
    triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23R01&res=1");
    request->send(200, "text/plain", "Preset 2 Triggered");
  });
  server.on("/preset3Action", HTTP_GET, [](AsyncWebServerRequest *request) {
    triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23R02&res=1");
    request->send(200, "text/plain", "Preset 3 Triggered");
  });
  server.on("/powerOnAction", HTTP_GET, [](AsyncWebServerRequest *request) {
    triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23O1&res=1");
    request->send(200, "text/plain", "Power ON Triggered");
  });
  server.on("/deskModeAction", HTTP_GET, [](AsyncWebServerRequest *request) {
    triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23INS0&res=1");
    request->send(200, "text/plain", "Desk Mode Triggered");
  });
  
  server.on("/updateSettings", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("ip", true)) {
      ptzCameraIP = request->getParam("ip", true)->value();
    }
    for (size_t i = 0; i < events.size(); ++i) {
      String paramNameStartDate = "startDate" + String(i);
      String paramNameStartTime = "startTime" + String(i);
      String paramNameStopTime = "stopTime" + String(i);
      if (request->hasParam(paramNameStartDate, true)) {
        events[i].date = request->getParam(paramNameStartDate, true)->value();
      }
      if (request->hasParam(paramNameStartTime, true)) {
        events[i].startTime = request->getParam(paramNameStartTime, true)->value();
      }
      if (request->hasParam(paramNameStopTime, true)) {
        events[i].stopTime = request->getParam(paramNameStopTime, true)->value();
      }
    }
    saveSettings();
    request->redirect("/");
  });
  
  server.begin();
  Serial.println("Async HTTP server started");
  
  // ----- Startup Sequence -----
  Serial.println("Sending Power ON command...");
  triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23O1&res=1");
  vTaskDelay(2000 / portTICK_PERIOD_MS);
  Serial.println("Sending Preset 1 command...");
  triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23R00&res=1");
  
  // ---- Initial Event Check ----
  time_t rawTime = timeClient.getEpochTime();
  time_t localTimeInit = usPacific.toLocal(rawTime);
  String currentDateInit = getFormattedDate(localTimeInit);
  int currentTotalMinutes = hour(localTimeInit) * 60 + minute(localTimeInit);
  bool shouldStream = false;
  for (const auto &event : events) {
    if (currentDateInit == event.date) {
      int startHour = event.startTime.substring(0, 2).toInt();
      int startMinute = event.startTime.substring(3, 5).toInt();
      int stopHour = event.stopTime.substring(0, 2).toInt();
      int stopMinute = event.stopTime.substring(3, 5).toInt();
      int startTotal = startHour * 60 + startMinute;
      int stopTotal = stopHour * 60 + stopMinute;
      if (currentTotalMinutes >= startTotal && currentTotalMinutes < stopTotal) {
        shouldStream = true;
        break;
      }
    }
  }
  if (shouldStream) {
    triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/rtmp_ctrl?cmd=start");
    Serial.println("Initial event check: within time window, starting stream.");
  } else {
    Serial.println("Initial event check: not within any event window.");
  }
  
  // Create the PTZ control task on core 1.
  xTaskCreatePinnedToCore(
    ptzControlTask,   // Task function
    "PTZ_Control",    // Task name
    8192,             // Stack size in words
    NULL,             // Parameter
    1,                // Priority
    NULL,             // Task handle
    1                 // Pin to core 1
  );
}

// ----- Main Loop -----
// With the PTZ control task on core 1 and async web server on core 0,
// the main loop simply yields.
void loop() {
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}
