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
#include <Timezone.h>

// Define time zones
TimeChangeRule usPDT = {"PDT", Second, Sun, Mar, 2, -420}; // Pacific Daylight Time = UTC - 7 hours
TimeChangeRule usPST = {"PST", First, Sun, Nov, 2, -480};  // Pacific Standard Time = UTC - 8 hours
Timezone usPacific(usPDT, usPST);

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

bool daylightSavingTime = false; // Added definition for daylightSavingTime

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

// Function to handle HTTP GET requests with retry logic
void triggerHttpGetWithRetry(String ipAddress, String command, int retries = 3) {
    int attempt = 0;
    bool success = false;
    while (attempt < retries && !success) {
        HTTPClient http;
        String url = "http://" + ipAddress + command;
        http.begin(url);
        int httpCode = http.GET();
        if (httpCode > 0) {
            Serial.println("HTTP GET request sent successfully.");
            success = true;
        } else {
            Serial.println("HTTP GET request failed. Retrying...");
            delay(1000); // Wait 1 second before retrying
            attempt++;
        }
        http.end();
    }
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
    unsigned long startTime = millis();
    const unsigned long timeout = 60000; // 1-minute timeout

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
            break; // Exit the loop when connected
        } else {
            Serial.println("Ethernet not connected. Retrying...");
        }

        if (millis() - startTime > timeout) {
            Serial.println("Ethernet not connected after timeout. Rebooting...");
            delay(5000); // Wait for 5 seconds before rebooting
            ESP.restart();
        }

        delay(500); // Wait half a second before retrying
    }
}


// Save settings and events to SPIFFS
void saveSettings() {
    StaticJsonDocument<2048> json; // Increased size to accommodate more events
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
        StaticJsonDocument<2048> json; // Increased size to accommodate more events
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
    delay(5000);  // Allow time for the router to initialize
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

    // HTTP server root page
    server.on("/", []() {
        Serial.println("Root page accessed");
        bool ntpUpdated = timeClient.update(); // Update the time from the NTP server
        if (ntpUpdated) {
            ntpUpdatedOnce = true; // Set flag if NTP update was successful
        }
        bool internetConnected = checkInternetConnectivity();

        // Get raw time from NTP
        time_t rawTime = timeClient.getEpochTime();
        // Adjust for selected timezone
        time_t localTime = usPacific.toLocal(rawTime);

        // Check PTZ connection status
        int rtmpStatus = getRTMPStatus(ptzCameraIP);
        String ptzStatus = (rtmpStatus != -1) ? "Connected" : "Not Connected";
        String streamStatus = (rtmpStatus == 1) ? "During Stream" : "Stream Suspended";

        // Start of HTML content
        String htmlPage = "<html><body>";
        htmlPage += "<h1>PTZ Stream Scheduler</h1>";
        htmlPage += "<p>Internet Connected: " + String(internetConnected ? "Yes" : "No") + "</p>";
        htmlPage += "<p>NTP Updated: " + String(ntpUpdatedOnce ? "Yes" : "No") + "</p>";
        htmlPage += "<p>Current Date: " + getFormattedDate(localTime) + "</p>";
        htmlPage += "<p>Current Time: " + getFormattedTime(localTime) + "</p>";
        htmlPage += "<form action='/updateSettings' method='post' id='settingsForm'>";

        // Loop through each event and add its settings
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

        // HTML to add a new event
        htmlPage += "<button type='button' onclick='addEvent()'>Add Event</button><br><br>";

        // PTZ Camera IP input field
        htmlPage += "<label for='ip'>PTZ Camera IP:</label><br>";
        htmlPage += "<input type='text' id='ip' name='ip' value='" + ptzCameraIP + "'><br><br>";

        // Submit button
        htmlPage += "<input type='submit' value='Update Settings'>";
        htmlPage += "</form>";

        // Current settings display
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

        // JavaScript for form actions
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
        htmlPage += "function triggerPreset1() {";
        htmlPage += "  var xhttp = new XMLHttpRequest();";
        htmlPage += "  xhttp.onreadystatechange = function() {";
        htmlPage += "    if (this.readyState == 4 && this.status == 200) {";
        htmlPage += "      console.log('Preset 1 triggered successfully');";
        htmlPage += "    }";
        htmlPage += "  };";
        htmlPage += "  xhttp.open('GET', '/preset1Action', true);";
        htmlPage += "  xhttp.send();";
        htmlPage += "}";
        htmlPage += "function triggerPreset2() {";
        htmlPage += "  var xhttp = new XMLHttpRequest();";
        htmlPage += "  xhttp.onreadystatechange = function() {";
        htmlPage += "    if (this.readyState == 4 && this.status == 200) {";
        htmlPage += "      console.log('Preset 2 triggered successfully');";
        htmlPage += "    }";
        htmlPage += "  };";
        htmlPage += "  xhttp.open('GET', '/preset2Action', true);";
        htmlPage += "  xhttp.send();";
        htmlPage += "}";
        htmlPage += "function triggerPreset3() {";
        htmlPage += "  var xhttp = new XMLHttpRequest();";
        htmlPage += "  xhttp.onreadystatechange = function() {";
        htmlPage += "    if (this.readyState == 4 && this.status == 200) {";
        htmlPage += "      console.log('Preset 3 triggered successfully');";
        htmlPage += "    }";
        htmlPage += "  };";
        htmlPage += "  xhttp.open('GET', '/preset3Action', true);";
        htmlPage += "  xhttp.send();";
        htmlPage += "}";
        htmlPage += "function triggerPowerOn() {";
        htmlPage += "  var xhttp = new XMLHttpRequest();";
        htmlPage += "  xhttp.onreadystatechange = function() {";
        htmlPage += "    if (this.readyState == 4 && this.status == 200) {";
        htmlPage += "      console.log('Power ON triggered successfully');";
        htmlPage += "    }";
        htmlPage += "  };";
        htmlPage += "  xhttp.open('GET', '/powerOnAction', true);";
        htmlPage += "  xhttp.send();";
        htmlPage += "}";
        htmlPage += "function triggerDeskMode() {";
        htmlPage += "  var xhttp = new XMLHttpRequest();";
        htmlPage += "  xhttp.onreadystatechange = function() {";
        htmlPage += "    if (this.readyState == 4 && this.status == 200) {";
        htmlPage += "      console.log('Desk Mode triggered successfully');";
        htmlPage += "    }";
        htmlPage += "  };";
        htmlPage += "  xhttp.open('GET', '/deskModeAction', true);";
        htmlPage += "  xhttp.send();";
        htmlPage += "}";
        htmlPage += "</script>";

        if (rtmpStatus != -1) {
          htmlPage += "<h3>PTZ Controls</h3>";
          htmlPage += "<button onclick=\"triggerPowerOn()\">Power ON</button><br><br>";
          htmlPage += "<button onclick=\"triggerPreset1()\">Preset 1</button><br><br>";
          htmlPage += "<button onclick=\"triggerPreset2()\">Preset 2</button><br><br>";
          htmlPage += "<button onclick=\"triggerPreset3()\">Preset 3</button><br><br>";
          htmlPage += "<button onclick=\"triggerDeskMode()\">Desk Mode</button><br><br>";
        }

        // End of HTML content
        htmlPage += "</body></html>";

        // Send the generated HTML page as a response to the client
        server.send(200, "text/html", htmlPage);
    });

    // Add event handler
    server.on("/addEvent", HTTP_POST, []() {
        Event newEvent = {"YYYY-MM-DD", "HH:MM", "HH:MM"}; // Default values for new events
        events.push_back(newEvent);
        saveSettings(); // Save the updated list to SPIFFS
        server.sendHeader("Location", "/", true); // Redirect back to root page
        server.send(302, "text/plain", "Event Added. Redirecting...");
    });

    // Delete event handler
    server.on("/deleteEvent", HTTP_POST, []() {
        if (server.hasArg("index")) {
            int index = server.arg("index").toInt();
            if (index >= 0 && index < events.size()) {
                events.erase(events.begin() + index);
                saveSettings(); // Save the updated list to SPIFFS
            }
        }
        server.sendHeader("Location", "/", true); // Redirect back to root page
        server.send(302, "text/plain", "Event Deleted. Redirecting...");
    });

    // Preset and PTZ action handlers
    server.on("/preset1Action", HTTP_GET, []() {
        triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23R00&res=1"); // Example command for Preset 1
        server.send(200, "text/plain", "Preset 1 Triggered");
    });

    server.on("/preset2Action", HTTP_GET, []() {
        triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23R01&res=1"); // Example command for Preset 2
        server.send(200, "text/plain", "Preset 2 Triggered");
    });

    server.on("/preset3Action", HTTP_GET, []() {
        triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23R02&res=1"); // Example command for Preset 3
        server.send(200, "text/plain", "Preset 3 Triggered");
    });

    server.on("/powerOnAction", HTTP_GET, []() {
        triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23O1&res=1"); // Example command for Power ON
        server.send(200, "text/plain", "Power ON Triggered");
    });

    server.on("/deskModeAction", HTTP_GET, []() {
        triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23INS0&res=1"); // Example command for Desk Mode
        server.send(200, "text/plain", "Desk Mode Triggered");
    });

    // Update settings handler
    server.on("/updateSettings", HTTP_POST, []() {
        // Update PTZ Camera IP
        if (server.hasArg("ip")) {
            ptzCameraIP = server.arg("ip");
        }

        // Update events
        for (size_t i = 0; i < events.size(); ++i) {
            if (server.hasArg("startDate" + String(i))) {
                events[i].date = server.arg("startDate" + String(i));
            }
            if (server.hasArg("startTime" + String(i))) {
                events[i].startTime = server.arg("startTime" + String(i));
            }
            if (server.hasArg("stopTime" + String(i))) {
                events[i].stopTime = server.arg("stopTime" + String(i));
            }
        }

        saveSettings(); // Save the updated settings to SPIFFS
        server.sendHeader("Location", "/", true); // Redirect back to root page
        server.send(302, "text/plain", "Settings Updated. Redirecting...");
    });

    server.begin();
    Serial.println("HTTP server started");

    // Test HTTP request to trigger a preset recall
    Serial.println("Testing HTTP request to trigger a preset recall...");
    triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/aw_ptz?cmd=%23R00&res=1");
}

void loop() {
    static bool previousRTMPStatus = -1;

    server.handleClient();

    // Update the time every 60 seconds
    if (millis() - lastCheckTime >= 60000) {
        time_t rawTime = timeClient.getEpochTime();
        time_t localTime = usPacific.toLocal(rawTime); // Adjusted to use usPacific.toLocal
        String currentDate = getFormattedDate(localTime);
        String currentTime = getFormattedTime(localTime);

        // Extract only hours and minutes for comparison
        String currentHourMinute = currentTime.substring(0, 5);

        for (const auto& event : events) {
            if (currentDate == event.date && currentHourMinute == event.startTime && !startCommandSent) {
                triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/rtmp_ctrl?cmd=start");
                startCommandSent = true;
                stopCommandSent = false; // Reset stop command status
            }

            if (currentHourMinute == event.stopTime && !stopCommandSent) {
                triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/rtmp_ctrl?cmd=stop");
                stopCommandSent = true;
                startCommandSent = false; // Reset start command status
            }
        }

        lastCheckTime = millis(); // Update last check time
    }

    // Check the RTMP status every 4 seconds
    if (millis() - lastCheckTime >= 4000) {
        int rtmpStatus = getRTMPStatus(ptzCameraIP);
        if (startCommandSent && rtmpStatus != 1) {
            triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/rtmp_ctrl?cmd=start");
        }
        if (stopCommandSent && rtmpStatus != 0) {
            triggerHttpGetWithRetry(ptzCameraIP, "/cgi-bin/rtmp_ctrl?cmd=stop");
        }

        // Refresh the page if the stream status changes
        if (rtmpStatus != previousRTMPStatus) {
            previousRTMPStatus = rtmpStatus;
        }

        lastCheckTime = millis(); // Update last check time
    }

    delay(1000);
}

          
