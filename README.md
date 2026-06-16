# Quiet Hours Generator Automation - Setup & Installation Guide

This guide details the physical wiring, controller configuration, Blynk IoT cloud settings, and fail-safe operations for the Quiet Hours Generator Automation system.

---

## 1. Physical Cabinet Wiring

All connections are made inside the generator AMF panel, directly tapping into the terminals at the back of the **Sedemac KG645C** controller and the ESP8266 module.

### A. Power Connections (12V DC System)
The ESP8266 board operates on a wide input range (7V–30V DC) and is powered directly from the generator's 12V starter battery.
1. Connect **KG645 Terminal 2 (BATT+)** through a **1A Inline Fuse** to the **VIN** terminal of the ESP8266 board.
2. Connect **KG645 Terminal 1 (BATT-)** directly to the **GND** terminal of the ESP8266 board.

### B. Simulate Mains Control Relay (GPIO4)
The relay controls the digital input on the controller to suppress starting.
1. Connect the relay **COM (Common)** terminal to **KG645 Terminal 1 (BATT-)** (or common GND).
2. Connect the relay **NO (Normally Open)** terminal to **KG645 Terminal 11 (DIG_IN_B)**.

### C. Generator Status Monitoring (GPIO5 via Opto-Isolator)
To monitor if the generator is running without risking electrical noise or starter cranking surges frying the ESP8266:
1. Connect **KG645 Terminal 3 (OUT A)** to a **1kΩ current-limiting resistor**, then to the **IN+ (Anode)** of the opto-isolator module.
2. Connect the **IN- (Cathode)** of the opto-isolator to **KG645 Terminal 1 (BATT-)** (or common GND).
3. Connect the **OUT (Collector)** pin of the opto-isolator to **GPIO5** on the ESP8266.
4. Connect the **GND (Emitter)** pin of the opto-isolator to **GND** on the ESP8266.

---

## 2. Sedemac KG645 Controller Settings

Configure the following parameters in the KG645 controller using the front keypad or the KG645 Configuration Utility:

### A. Digital Input B (Simulate Mains)
*   **Source (SOURCE)**: Set to `14: Simulate Mains` (confirms to the controller that mains voltage is healthy).
*   **Polarity (POLARITY)**: Set to `Close To Activate` (active when connected to BATT-).
*   **Action (ACTION)**: Set to `None`.

### B. Digital Output A (Generator Available Status)
*   **Source (SOURCE)**: Set to `28: Generator available (Gen Available)` (active when engine is running and alternator voltage/frequency are healthy).
*   **On Activation (ON ACTIVATION)**: Set to `Energise` (outputs BATT+ to drive the opto-isolator input when running).

---

## 3. Blynk IoT Cloud Setup

### A. Device Credentials
Ensure the following credentials (provided in `relay_board_pinout.txt`) are defined in the firmware:
```cpp
#define BLYNK_TEMPLATE_ID   "TMPL3I1Y7fT0q"
#define BLYNK_TEMPLATE_NAME "Genset Controller"
#define BLYNK_AUTH_TOKEN    "M6Rt4jiHD8lXyszSM7nJO-y3xGslihxg"
```

### B. Datastream Map
Configure the virtual pins in the Blynk IoT Console under your device template:

| Pin | Name | Data Type | Limits / Units | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **V4** | Simulate Mains | Integer | `0` to `1` (Default: `0`) | Controls the Night Mode relay (1 = ON/Suppressed, 0 = OFF/Normal). |
| **V5** | Dig In | Integer | `0` to `1` | Generator running status (1 = Running, 0 = Stopped). |
| **V6** | Terminal Log | String | N/A | Diagnostics and system boot/NTP sync event logs. |
| **V7** | System Status | String | N/A | Current mode status (e.g. "Night Mode Active"). |
| **V8** | WiFi RSSI | Integer | `-100` to `0` dBm | Displays local WiFi signal strength. |

### C. Blynk Event & Notification Configuration
To receive push notifications on your mobile app, navigate to **Templates** -> **Events** and create these two events:

1.  **Event Code**: `generator_status`
    *   **Event Name**: Generator Status Update
    *   **Type**: Info
    *   **Notification**: Enable "Send push notification" to the Blynk app.
    *   **Notification Message**: `{VAL}` (will dynamically display "Generator STARTED running" or "Generator STOPPED...").

2.  **Event Code**: `generator_warning`
    *   **Event Name**: Continuous Runtime Warning
    *   **Type**: Warning
    *   **Notification**: Enable "Send push notification" (High Priority).
    *   **Notification Message**: `CRITICAL: Generator has been running continuously for more than 2 hours!`

---

## 4. Boot-Time & Fail-Safe Operation

### A. Hardware Fail-Safe (Normal AMF Recovery)
On ESP8266 reboot, power loss, or firmware lockup, the relay **defaults to OFF** (contacts open). Because the KG645 is configured with `Close To Activate` polarity, the open-circuit default immediately de-activates `Simulate Mains`, restoring full native AMF functionality. The generator will always be able to start during outages even if the ESP8266 fails.

### B. Boot-Time State Syncing & NTP Scheduler
1.  **Offline Boot**: The relay remains OFF. The generator acts in normal AMF mode.
2.  **Network Connect & Time Sync**: Once the ESP8266 connects to WiFi and syncs its local time via NTP:
    *   If the local time is within **Quiet Hours (11:00 PM – 6:00 AM)**, the local scheduler **automatically overrides** the default state and activates Night Mode (Relay ON).
    *   If the time is outside Quiet Hours, it keeps Night Mode OFF.
3.  **Blynk Connection Sync**: Once Blynk connects, it syncs with the app dashboard. If you need to run the generator during the night, toggling the Night Mode switch OFF in the Blynk app will temporarily override the scheduler and restore AMF start.

### C. WiFi Setup & Captive Portal (WiFiManager)
If you ever change your router's name or password, you do **not** need to reprogram the board.
1. On initial boot (or if the home WiFi is unreachable), the ESP8266 will create a temporary WiFi access point named **"Genset-Controller-Setup"**.
2. Connect your smartphone or computer to this network. A portal webpage will open automatically (if it doesn't, open a browser and go to `192.168.4.1`).
3. Scan for networks, select your new WiFi network, enter the password, and click **Save**.
4. The ESP8266 will store these credentials permanently, reboot, and connect.
5. **Auto-Timeout**: If your home router is booting up slowly (e.g. after a power cut), the ESP8266 setup portal will timeout after 3 minutes and boot into normal offline mode, ensuring the system remains operational and fail-safe.

---

## 5. Remote Firmware Updates (Blynk.Air)

The ESP8266 is configured to support remote wireless firmware updates via **Blynk.Air** (Blynk Cloud OTA). This allows you to upgrade the firmware without physically connecting the board to your computer via USB.

### How to Perform a Remote Update:

1. **Increment the Firmware Version**:
   * Open the sketch file [genset_controller.ino](file:///c:/orbit/git_repos/genset-controller/genset_controller/genset_controller.ino).
   * Find the `#define BLYNK_FIRMWARE_VERSION "1.0.0"` line at the top.
   * Change it to a higher version number (e.g., `"1.0.1"`). The Blynk server will ignore the update if the version number is not incremented.

2. **Export Compiled Binary in Arduino IDE**:
   * In the Arduino IDE, go to the top menu and select **Sketch** -> **Export Compiled Binary**.
   * Wait for compilation to finish.
   * In your project directory, a `.bin` file will be generated (typically in the same folder as your `.ino` file, or inside a `build` subdirectory).

3. **Ship Firmware via Blynk Console**:
   * Log into the **Blynk Cloud Console** (https://blynk.cloud).
   * Navigate to the **Blynk.Air** section (or the **Firmware** tab under your Device / Device Template).
   * Upload the exported `.bin` file.
   * Enter the new version number matching your code definition (e.g., `1.0.1`).
   * Select your target device and trigger the update by clicking **Ship**.
   * The ESP8266 will automatically download the binary file, perform internal checksum/integrity validation, flash it, and reboot to run the new firmware. Progress and statuses are logged to the Serial monitor and to the Blynk Virtual Terminal (V6) before disconnecting.
