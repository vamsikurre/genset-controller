/********************************************************************************
 * Project: Quiet Hours Generator Automation
 * Device: ESP8266-12E WiFi Relay Module
 * Controller: Sedemac KG645 (with Negative Sensing inputs)
 * Platform: Blynk IoT & ESP8266 Arduino Framework
 *
 * Pin Map:
 *   - GPIO4 (D2) -> Onboard Relay (Active HIGH) -> KG645 Terminal 11 (DIG_IN_B)
 *   - GPIO5 (D1) -> Onboard Switch/Sensor (Active LOW) -> Opto-isolator OUT for OUT A (Gen Available)
 *   - GPIO2 (D4) -> Onboard Status LED (Active LOW)
 ********************************************************************************/

#include "config.h"
#define BLYNK_FIRMWARE_VERSION "1.0.0"

#define BLYNK_PRINT Serial

#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <WiFiManager.h> // WiFiManager library for captive portal provisioning
#include <WiFiClientSecure.h>
#include <ESP8266httpUpdate.h>

// --- PIN DEFINITIONS ---
const int RELAY_PIN = 4; // GPIO4 (Controls Simulate Mains input)
const int INPUT_PIN = 5; // GPIO5 (Monitors Generator Running status)
const int LED_PIN   = 2; // GPIO2 (Onboard LED)

// --- CONFIGURATION ---
// WiFi credentials are now managed dynamically via WiFiManager (captive portal)

// Timezone Settings (India Standard Time: UTC +5:30)
const long utcOffsetInSeconds = 19800; // 5 hours 30 mins * 3600

// --- STATE VARIABLES ---
bool autoMode = true;             // Automatic scheduler mode (true = Auto, false = Manual)
bool nightMode = false;           // Target State (true = relay ON, false = relay OFF)
bool currentRelayState = false;   // Actual hardware relay state
bool genRunning = false;          // Generator running state (from GPIO5)
bool timeSynced = false;          // True once NTP successfully syncs at least once
unsigned long genStartTime = 0;   // Track when generator started
bool longRunWarningSent = false;  // Flag to avoid repeating alerts

// --- TIME CONTROLS ---
int quietStartHour = 23;          // Quiet start hour (0-23, default 11:00 PM)
int quietEndHour   = 6;           // Quiet end hour (0-23, default 06:00 AM)

// --- CLASS INSTANCES ---
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds, 60000); // Update every 60s
BlynkTimer timer;

// Non-blocking Connection Tracking
unsigned long lastWiFiCheck = 0;
unsigned long lastBlynkCheck = 0;
const unsigned long wifiRetryInterval = 10000;  // Check WiFi status every 10s
const unsigned long blynkRetryInterval = 15000; // Check Blynk status every 15s

// Event logging helper
void logToTerminal(const String& message) {
  Serial.println(message);
  if (Blynk.connected()) {
    // Send to Virtual Terminal widget (V6)
    Blynk.virtualWrite(V6, message + "\n");
  }
}

// Update status message on Blynk
void updateBlynkStatus() {
  if (Blynk.connected()) {
    String statusStr;
    if (autoMode) {
      statusStr = "Auto Mode: " + String(nightMode ? "Night Mode Active (Genset Suppressed)" : "AMF Active (Genset Normal)");
    } else {
      statusStr = "Manual Mode: " + String(currentRelayState ? "Mains Simulated (Genset Suppressed)" : "AMF Active (Genset Normal)");
    }
    Blynk.virtualWrite(V7, statusStr);
    Blynk.virtualWrite(V8, WiFi.RSSI());
  }
}

// Drive Relay and LED
void applyRelayState(bool enable, const String& source) {
  nightMode = enable;
  
  if (nightMode != currentRelayState) {
    currentRelayState = nightMode;
    digitalWrite(RELAY_PIN, currentRelayState ? HIGH : LOW);
    
    // Sync Widget switch
    Blynk.virtualWrite(V4, currentRelayState ? 1 : 0);
    
    String msg = "Night Mode " + String(currentRelayState ? "ENABLED" : "DISABLED") + " by " + source;
    logToTerminal(msg);
    updateBlynkStatus();
  }
}

// Non-blocking WiFi & Blynk Connection Manager
void maintainConnections() {
  unsigned long now = millis();

  // 1. Maintain WiFi Connection
  if (WiFi.status() != WL_CONNECTED) {
    return; // Stop here if WiFi is down, the SDK auto-reconnects in the background
  }

  // 2. Maintain Blynk Connection
  if (!Blynk.connected()) {
    if (now - lastBlynkCheck >= blynkRetryInterval) {
      lastBlynkCheck = now;
      Serial.println("Blynk disconnected. Attempting to connect...");
      Blynk.connect();
    }
  } else {
    Blynk.run();
  }
}

// Blynk Switch handler (V4)
BLYNK_WRITE(V4) {
  int val = param.asInt();
  
  // If we are currently in Auto Mode and the user toggles the relay, disable Auto Mode
  if (autoMode) {
    autoMode = false;
    Blynk.virtualWrite(V3, 0); // Update Auto Mode widget to OFF
    logToTerminal("Manual override detected. Auto Mode DISABLED.");
  }
  
  applyRelayState(val == 1, "Blynk App Manual Override");
}

// Blynk Auto Mode Switch handler (V3)
BLYNK_WRITE(V3) {
  int val = param.asInt();
  autoMode = (val == 1);
  logToTerminal("Auto Mode set to " + String(autoMode ? "ENABLED" : "DISABLED") + " via Blynk.");
  
  if (autoMode) {
    // Immediately run scheduler to enforce correct state
    checkTimeScheduler();
  } else {
    // In manual mode, sync relay state from the Blynk cloud
    Blynk.syncVirtual(V4);
  }
  updateBlynkStatus();
}

// Blynk Onboard LED Switch handler (V2)
BLYNK_WRITE(V2) {
  int val = param.asInt();
  digitalWrite(LED_PIN, val == 1 ? LOW : HIGH); // Active LOW LED
}

// Blynk Quiet Hours Start Hour handler (V9)
BLYNK_WRITE(V9) {
  int val = param.asInt();
  if (val >= 0 && val <= 23) {
    quietStartHour = val;
    logToTerminal("Quiet Start Hour updated to: " + String(quietStartHour) + ":00");
    if (autoMode) {
      checkTimeScheduler();
    }
  }
}

// Blynk Quiet Hours End Hour handler (V10)
BLYNK_WRITE(V10) {
  int val = param.asInt();
  if (val >= 0 && val <= 23) {
    quietEndHour = val;
    logToTerminal("Quiet End Hour updated to: " + String(quietEndHour) + ":00");
    if (autoMode) {
      checkTimeScheduler();
    }
  }
}

// Sync Virtual Pin state on connection
BLYNK_CONNECTED() {
  logToTerminal("Blynk Connected successfully.");
  Blynk.syncVirtual(V3); // Sync Auto Mode state (which determines if we sync V4 or push to V4)
  Blynk.syncVirtual(V2); // Sync LED state
  Blynk.syncVirtual(V9); // Sync Quiet Start Hour
  Blynk.syncVirtual(V10); // Sync Quiet End Hour
  updateBlynkStatus();
}

// Blynk.Air Remote OTA update handler
BLYNK_WRITE(InternalPinOTA) {
  String overTheAirURL = param.asString();
  logToTerminal("Blynk.Air: OTA update triggered! URL: " + overTheAirURL);

  // Disconnect from Blynk to prevent network activity interference
  Blynk.disconnect();

  WiFiClientSecure clientSecure;
  WiFiClient clientHttp;

  clientSecure.setInsecure(); // Disable SSL validation for simplicity on ESP8266

  t_httpUpdate_return ret;
  if (overTheAirURL.startsWith("https://")) {
    ret = ESPhttpUpdate.update(clientSecure, overTheAirURL);
  } else {
    ret = ESPhttpUpdate.update(clientHttp, overTheAirURL);
  }

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      logToTerminal("Blynk.Air: Update FAILED! Error " + String(ESPhttpUpdate.getLastError()) + ": " + ESPhttpUpdate.getLastErrorString());
      // Attempt to reconnect to Blynk
      Blynk.connect();
      break;
    case HTTP_UPDATE_NO_UPDATES:
      logToTerminal("Blynk.Air: No updates available.");
      Blynk.connect();
      break;
    case HTTP_UPDATE_OK:
      logToTerminal("Blynk.Air: Update successful. MCU rebooting...");
      ESP.restart();
      break;
  }
}

// Helper to determine if current hour is within quiet hours (handles midnight spanning)
bool isWithinQuietHours(int currentHour, int startHour, int endHour) {
  if (startHour == endHour) {
    return false;
  }
  if (startHour < endHour) {
    return (currentHour >= startHour && currentHour < endHour);
  } else {
    return (currentHour >= startHour || currentHour < endHour);
  }
}

// NTP Fallback Scheduler
void checkTimeScheduler() {
  // Call update to request sync. NTPClient automatically rate-limits requests internally.
  timeClient.update();

  // If the clock has not been synchronized at least once yet, wait until it is
  if (timeClient.getEpochTime() < 100000UL) {
    Serial.println("NTP Time not yet synchronized. Waiting for network...");
    return;
  }

  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  
  if (!timeSynced) {
    timeSynced = true;
    char timeStr[10];
    sprintf(timeStr, "%02d:%02d", currentHour, currentMinute);
    logToTerminal("NTP Time synchronized. Current Time: " + String(timeStr));
  }

  // If in Auto Mode, continuously enforce the quiet hours schedule
  if (autoMode) {
    bool shouldBeOn = isWithinQuietHours(currentHour, quietStartHour, quietEndHour);
    applyRelayState(shouldBeOn, "Local NTP Scheduler");
  }
}

// Monitor GPIO5 for Generator Running State
void checkGeneratorStatus() {
  // Opto-Isolator outputs LOW when the generator runs (connected to OUT A / Gen Available)
  bool isRunningNow = (digitalRead(INPUT_PIN) == LOW);

  if (isRunningNow && !genRunning) {
    // Generator started
    genRunning = true;
    genStartTime = millis();
    longRunWarningSent = false;
    
    logToTerminal("Generator Alert: Generator STARTED running.");
    Blynk.virtualWrite(V5, 1);
    
    if (Blynk.connected()) {
      Blynk.logEvent("generator_status", "Generator has STARTED running.");
    }
  } 
  else if (!isRunningNow && genRunning) {
    // Generator stopped
    genRunning = false;
    unsigned long runDurationMs = millis() - genStartTime;
    unsigned long runMinutes = runDurationMs / (60 * 1000);
    
    String msg = "Generator Alert: Generator STOPPED. Total Run Duration: " + String(runMinutes) + " minute(s).";
    logToTerminal(msg);
    Blynk.virtualWrite(V5, 0);
    
    if (Blynk.connected()) {
      Blynk.logEvent("generator_status", msg);
    }
  }

  // Warning alert for long run times (> 2 hours)
  if (genRunning && !longRunWarningSent) {
    unsigned long runDurationMs = millis() - genStartTime;
    if (runDurationMs > 2UL * 60UL * 60UL * 1000UL) { // 2 hours in ms
      longRunWarningSent = true;
      String msg = "CRITICAL: Generator has been running continuously for more than 2 hours!";
      logToTerminal(msg);
      if (Blynk.connected()) {
        Blynk.logEvent("generator_warning", msg);
      }
    }
  }
}

// Telemetry timer event (RSSI and connection updates)
void checkTelemetry() {
  if (WiFi.status() == WL_CONNECTED) {
    updateBlynkStatus();
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nBooting Genset Controller...");

  // --- GPIO INITIALIZATION ---
  // Ensure relay is strictly LOW on boot (Fail-Safe: normal AMF active)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  currentRelayState = false;

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Onboard LED off (Active LOW)

  pinMode(INPUT_PIN, INPUT_PULLUP); // Onboard sensor input with pull-up

  // --- NETWORK INITIALIZATION ---
  WiFi.mode(WIFI_STA);
  
  WiFiManager wm;
  // Set config portal timeout to 180 seconds (3 minutes) to ensure it doesn't block forever
  wm.setConfigPortalTimeout(180);
  
  Serial.println("Connecting to WiFi...");
  // autoConnect will try to connect using saved credentials.
  // If it fails, it will start a temporary Access Point named "Genset-Controller-Setup"
  if (!wm.autoConnect("Genset-Controller-Setup")) {
    Serial.println("Failed to connect and hit portal timeout. Continuing in offline mode...");
  } else {
    Serial.println("WiFi connected successfully!");
  }
  
  // Blynk config (non-blocking)
  Blynk.config(BLYNK_AUTH_TOKEN);
  
  // Start NTP
  timeClient.begin();

  // --- TIMER EVENTS ---
  // Check NTP time/scheduler every 10 seconds (non-blocking)
  timer.setInterval(10000L, checkTimeScheduler);
  
  // Check generator running status on GPIO5 every 500ms
  timer.setInterval(500L, checkGeneratorStatus);
  
  // Telemetry updates every 30 seconds
  timer.setInterval(30000L, checkTelemetry);

  logToTerminal("System boot completed. Initializing connection...");
}

void loop() {
  maintainConnections();
  timer.run();
}
