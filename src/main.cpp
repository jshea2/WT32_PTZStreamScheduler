#include <ETH.h>
#include <WebServer_WT32_ETH01.h>
#include <TimeLib.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <HTTPClient.h>
#include <vector>
#include <SPIFFS.h>
#include <ArduinoJson.h>

#define ETH_ADDR        1
#define ETH_POWER_PIN   16
#define ETH_MDC_PIN     23
#define ETH_MDIO_PIN    18
#define ETH_TYPE        ETH_PHY_LAN8720
#define ETH_CLK_MODE    ETH_CLOCK_GPIO17_OUT

IPAddress local_IP(10, 0, 3, 3);
IPAddress gateway(10, 0, 3, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns1(8, 8, 8, 8); // Google's public DNS
IPAddress dns2(8, 8, 4, 4); // Google's public DNS

WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000); // NTP server for GMT

long utcOffsetInSeconds = -8 * 3600; // Default to PST (-8 hours)
bool daylightSavingTime = false; // Daylight Saving Time flag
bool ntpUpdatedOnce = false; // Track if NTP update was successful once

struct Event {
    String date;
    String startTime;
    String stopTime;
};

String ptzCameraIP = "10.0.3.61";
bool startCommandSent = false;
bool stopCommandSent = false;
unsigned long lastCheckTime = 0;

std::vector<Event> events;

// Function to format the time in HH:MM format
String getFormattedTime(time_t rawTime) {
    char timeStr[6];
    sprintf(timeStr, "%02d:%02d", hour(rawTime), minute(rawTime));
    return String(timeStr);
}

// Function to format the date in YYYY-MM-DD format
String getFormattedDate(time_t rawTime) {
    char dateStr[11];
    sprintf(dateStr, "%04d-%02d-%02d", year(rawTime), month(rawTime), day(rawTime));
    return String(dateStr);
}

// Function to check internet connectivity by making an HTTP request
bool checkInternetConnectivity() {
    HTTPClient http;
    http.begin("http://clients3.google.com/generate_204");
    int httpCode = http.GET();
    http.end();
    return (httpCode == 204);
}

// Function to handle HTTP GET requests
void triggerHttpGet(String ipAddress, String command) {
    HTTPClient http;
    String url = "http://" + ipAddress + command;
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode > 0) {
        Serial.println("HTTP GET request sent successfully.");
    } else {
        Serial.println("HTTP GET request failed.");
    }
    http.end();
}

// Function to check RTMP status
int getRTMPStatus(String ipAddress) {
    HTTPClient http;
    String url = "http://" + ipAddress + "/cgi-bin/get_rtmp_status";
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

void connectEthernet() {
    while (!ETH.linkUp()) {
        Serial.print(".");
        delay(1000);
    }
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
}

// Save settings and events to SPIFFS
void saveSettings() {
    StaticJsonDocument<1024> json;
    json["ptzCameraIP"] = ptzCameraIP;
    json["utcOffsetInSeconds"] = utcOffsetInSeconds;
    json["daylightSavingTime"] = daylightSavingTime;

    JsonArray eventsArray = json.createNestedArray("events");
    for (const auto& event : events) {
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

// Load settings and events from SPIFFS
void loadSettings() {
    File file = SPIFFS.open("/settings.json", FILE_READ);
    if (file) {
        StaticJsonDocument<1024> json;
        DeserializationError error = deserializeJson(json, file);
        if (!error) {
            ptzCameraIP = json["ptzCameraIP"].as<String>();
            utcOffsetInSeconds = json["utcOffsetInSeconds"];
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

void setup() {
    pinMode(ETH_POWER_PIN, OUTPUT);
    digitalWrite(ETH_POWER_PIN, HIGH);

    Serial.begin(115200);
    delay(1000);
    Serial.println("Starting...");

    if (!SPIFFS.begin(true)) {
        Serial.println("An error has occurred while mounting SPIFFS");
    }

    loadSettings(); // Load settings from SPIFFS

    ETH.begin(ETH_ADDR, ETH_POWER_PIN, ETH_MDC_PIN, ETH_MDIO_PIN, ETH_TYPE, ETH_CLK_MODE);

    // Static IP configuration with DNS settings
    if (!ETH.config(local_IP, gateway, subnet, dns1, dns2)) {
        Serial.println("Failed to configure Ethernet.");
    } else {
        Serial.println("Ethernet configured successfully.");
    }

    // Retry Ethernet connection until successful
    connectEthernet();

    // Initialize NTP client
    timeClient.begin();

    server.on("/", []() {
        bool ntpUpdated = timeClient.update(); // Update the time from the NTP server
        if (ntpUpdated) {
            ntpUpdatedOnce = true; // Set flag if NTP update was successful
        }
        bool internetConnected = checkInternetConnectivity();

        // Get raw time from NTP
        time_t rawTime = timeClient.getEpochTime();
        // Adjust for selected timezone and daylight saving time
        time_t adjustedTime = rawTime + utcOffsetInSeconds + (daylightSavingTime ? 3600 : 0);

        // Check PTZ connection status
        int rtmpStatus = getRTMPStatus(ptzCameraIP);
        String ptzStatus = (rtmpStatus != -1) ? "Connected" : "Not Connected";
        String streamStatus = (rtmpStatus == 1) ? "During Stream" : "Stream Suspended";

        String htmlPage = "<html><body>";
        htmlPage += "<h1>PTZ Stream Scheduler</h1>";
        htmlPage += "<p>Internet Connected: " + String(internetConnected ? "Yes" : "No") + "</p>";
        htmlPage += "<p>NTP Updated: " + String(ntpUpdatedOnce ? "Yes" : "No") + "</p>";
        htmlPage += "<p>Current Date: " + getFormattedDate(adjustedTime) + "</p>";
        htmlPage += "<p>Current Time: " + getFormattedTime(adjustedTime) + "</p>";
        htmlPage += "<form action='/updateSettings' method='post' id='settingsForm'>";
        for (size_t i = 0; i < events.size(); ++i) {
            htmlPage += "<div id='event" + String(i) + "'>";
            htmlPage += "<h2>Event " + String(i + 1) + "</h2>";
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
        htmlPage += "<label for='timezone'>Select Timezone:</label><br>";
        htmlPage += "<select id='timezone' name='timezone'>";
        htmlPage += "<option value='-12'>GMT-12</option>";
        htmlPage += "<option value='-11'>GMT-11</option>";
        htmlPage += "<option value='-10'>GMT-10</option>";
        htmlPage += "<option value='-9'>GMT-9</option>";
        htmlPage += "<option value='-8' selected>PST (GMT-8)</option>";
        htmlPage += "<option value='-7'>MST (GMT-7)</option>";
        htmlPage += "<option value='-6'>CST (GMT-6)</option>";
        htmlPage += "<option value='-5'>EST (GMT-5)</option>";
        htmlPage += "<option value='-4'>GMT-4</option>";
        htmlPage += "<option value='-3'>GMT-3</option>";
        htmlPage += "<option value='-2'>GMT-2</option>";
        htmlPage += "<option value='-1'>GMT-1</option>";
        htmlPage += "<option value='0'>GMT</option>";
        htmlPage += "<option value='1'>GMT+1</option>";
        htmlPage += "<option value='2'>GMT+2</option>";
        htmlPage += "<option value='3'>GMT+3</option>";
        htmlPage += "<option value='4'>GMT+4</option>";
        htmlPage += "<option value='5'>GMT+5</option>";
        htmlPage += "<option value='6'>GMT+6</option>";
        htmlPage += "<option value='7'>GMT+7</option>";
        htmlPage += "<option value='8'>GMT+8</option>";
        htmlPage += "<option value='9'>GMT+9</option>";
        htmlPage += "<option value='10'>GMT+10</option>";
        htmlPage += "<option value='11'>GMT+11</option>";
        htmlPage += "<option value='12'>GMT+12</option>";
        htmlPage += "</select><br><br>";
        htmlPage += "<label for='dst'>Daylight Saving Time (+1hr):</label><br>";
        htmlPage += "<input type='checkbox' id='dst' name='dst' " + String(daylightSavingTime ? "checked" : "") + " onchange='updateDST()'><br><br>";
        htmlPage += "<input type='submit' value='Update Settings'>";
        htmlPage += "</form>";

        // Add current settings
        htmlPage += "<h2>Current Settings</h2>";
        for (size_t i = 0; i < events.size(); ++i) {
            htmlPage += "<p>Event " + String(i + 1) + ":</p>";
            htmlPage += "<p>Start Date: " + events[i].date + "</p>";
            htmlPage += "<p>Start Time: " + events[i].startTime + "</p>";
            htmlPage += "<p>Stop Time: " + events[i].stopTime + "</p>";
        }
        htmlPage += "<p>PTZ Camera IP: " + ptzCameraIP + "</p>";
        htmlPage += "<p>PTZ: " + ptzStatus + "</p>";
        htmlPage += "<p>Stream Status: " + streamStatus + "</p>";

        htmlPage += "<script>";
        htmlPage += "function addEvent() {";
        htmlPage += "  var form = document.getElementById('settingsForm');";
        htmlPage += "  form.action = '/addEvent';";
        htmlPage += "  form.submit();";
        htmlPage += "}";
        htmlPage += "function deleteEvent(index) {";
        htmlPage += "  var form = document.getElementById('settingsForm');";
        htmlPage += "  form.action = '/deleteEvent?index=' + index;";
        htmlPage += "  form.submit();";
        htmlPage += "}";
        htmlPage += "function updateDST() {";
        htmlPage += "  var form = document.getElementById('settingsForm');";
        htmlPage += "  form.action = '/updateDST';";
        htmlPage += "  form.submit();";
        htmlPage += "}";
        htmlPage += "</script>";
        htmlPage += "</body></html>";
        server.send(200, "text/html", htmlPage);
    });

    server.on("/updateSettings", []() {
        events.clear();
        int i = 0;
        while (server.hasArg("startDate" + String(i))) {
            Event event;
            event.date = server.arg("startDate" + String(i));
            event.startTime = server.arg("startTime" + String(i));
            event.stopTime = server.arg("stopTime" + String(i));
            events.push_back(event);
            i++;
        }
        if (server.hasArg("ip")) {
            ptzCameraIP = server.arg("ip");
        }
        if (server.hasArg("timezone")) {
            utcOffsetInSeconds = server.arg("timezone").toInt() * 3600;
        }
        if (server.hasArg("dst")) {
            daylightSavingTime = server.arg("dst") == "on";
        } else {
            daylightSavingTime = false;
        }
        saveSettings(); // Save settings to SPIFFS
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
    });

    server.on("/updateDST", []() {
        if (server.hasArg("dst")) {
            daylightSavingTime = server.arg("dst") == "on";
        } else {
            daylightSavingTime = false;
        }
        saveSettings(); // Save settings to SPIFFS
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
    });

    server.on("/addEvent", []() {
        events.push_back(Event{"", "", ""});
        saveSettings(); // Save settings to SPIFFS
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
    });

    server.on("/deleteEvent", []() {
        if (server.hasArg("index")) {
            int index = server.arg("index").toInt();
            if (index >= 0 && index < events.size()) {
                events.erase(events.begin() + index);
            }
        }
        saveSettings(); // Save settings to SPIFFS
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
    });

    server.begin();
    Serial.println("HTTP server started");

    // Test HTTP request to trigger a preset recall
    Serial.println("Testing HTTP request to trigger a preset recall...");
    triggerHttpGet(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23R00&res=1");
}

void loop() {
    static bool previousRTMPStatus = -1;

    server.handleClient();

    time_t rawTime = timeClient.getEpochTime();
    time_t adjustedTime = rawTime + utcOffsetInSeconds + (daylightSavingTime ? 3600 : 0);
    String currentDate = getFormattedDate(adjustedTime);
    String currentTime = getFormattedTime(adjustedTime);

    // Extract only hours and minutes for comparison
    String currentHourMinute = currentTime.substring(0, 5);

    for (const auto& event : events) {
        if (currentDate == event.date && currentHourMinute == event.startTime && !startCommandSent) {
            triggerHttpGet(ptzCameraIP, "/cgi-bin/rtmp_ctrl?cmd=start");
            startCommandSent = true;
            stopCommandSent = false; // Reset stop command status
            lastCheckTime = millis(); // Reset last check time
        }

        if (currentHourMinute == event.stopTime && !stopCommandSent) {
            triggerHttpGet(ptzCameraIP, "/cgi-bin/rtmp_ctrl?cmd=stop");
            stopCommandSent = true;
            startCommandSent = false; // Reset start command status
            lastCheckTime = millis(); // Reset last check time
        }
    }

    // Check the RTMP status every 4 seconds
    if (millis() - lastCheckTime >= 4000) {
        int rtmpStatus = getRTMPStatus(ptzCameraIP);
        if (startCommandSent && rtmpStatus != 1) {
            triggerHttpGet(ptzCameraIP, "/cgi-bin/rtmp_ctrl?cmd=start");
        }
        if (stopCommandSent && rtmpStatus != 0) {
            triggerHttpGet(ptzCameraIP, "/cgi-bin/rtmp_ctrl?cmd=stop");
        }

        // Refresh the page if the stream status changes
        if (rtmpStatus != previousRTMPStatus) {
            previousRTMPStatus = rtmpStatus;
        }

        lastCheckTime = millis(); // Update last check time
    }

    delay(1000);
}
