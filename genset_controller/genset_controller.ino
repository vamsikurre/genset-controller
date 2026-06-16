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
bool nightMode = false;           // Target State (true = relay ON, false = relay OFF)
bool currentRelayState = false;   // Actual hardware relay state
bool genRunning = false;          // Generator running state (from GPIO5)
bool timeSynced = false;          // True once NTP successfully syncs at least once
unsigned long genStartTime = 0;   // Track when generator started
bool longRunWarningSent = false;  // Flag to avoid repeating alerts

// --- TIME CONTROLS ---
const int QUIET_START_HOUR = 23; // 11:00 PM
const int QUIET_END_HOUR   = 6;  // 06:00 AM

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
    if (nightMode) {
      statusStr = "Night Mode Active (Genset Suppressed)";
    } else {
      statusStr = "AMF Active (Genset Normal)";
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
    digitalWrite(LED_PIN, currentRelayState ? LOW : HIGH); // Active LOW LED
    
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
  applyRelayState(val == 1, "Blynk App Manual Toggle");
}

// Sync Virtual Pin state on connection
BLYNK_CONNECTED() {
  logToTerminal("Blynk Connected successfully.");
  Blynk.syncVirtual(V4);
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

// NTP Fallback Scheduler
void checkTimeScheduler() {
  if (!timeClient.update()) {
    Serial.println("NTP update failed. Retrying...");
    return;
  }

  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  
  // Format local time string
  char timeStr[10];
  sprintf(timeStr, "%02d:%02d", currentHour, currentMinute);
  
  if (!timeSynced) {
    timeSynced = true;
    logToTerminal("NTP Time synchronized. Current Time: " + String(timeStr));
    
    // Boot-up rule: If time is already within Quiet Hours, force Night Mode active
    if (currentHour >= QUIET_START_HOUR || currentHour < QUIET_END_HOUR) {
      logToTerminal("Time falls within Quiet Hours at boot. Enforcing Night Mode.");
      applyRelayState(true, "Local NTP Boot-up Check");
    } else {
      logToTerminal("Time falls outside Quiet Hours. Normal AMF restored.");
      applyRelayState(false, "Local NTP Boot-up Check");
    }
    return;
  }

  // Periodic Transition checks
  // 1. At exactly 11:00 PM, turn Night Mode ON
  if (currentHour == QUIET_START_HOUR && currentMinute == 0 && !nightMode) {
    logToTerminal("Scheduled event: 11:00 PM reached.");
    applyRelayState(true, "Local NTP Scheduler Transition");
  }
  // 2. At exactly 6:00 AM, turn Night Mode OFF
  else if (currentHour == QUIET_END_HOUR && currentMinute == 0 && nightMode) {
    logToTerminal("Scheduled event: 06:00 AM reached.");
    applyRelayState(false, "Local NTP Scheduler Transition");
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
