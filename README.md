# Quiet Hours Generator Automation - Setup & Installation Guide

This guide details the physical wiring, controller configuration, Blynk IoT cloud settings, and fail-safe operations for the Quiet Hours Generator Automation system.

---

## 1. Physical Cabinet Wiring

All connections are made inside the generator AMF panel, directly tapping into the terminals at the back of the **Sedemac KG645C** controller and the ESP8266 module.

### A. Power Connections (12V DC System)
The ESP8266 board operates on a wide input range (7V–30V DC) and is powered directly from the generator's 12V starter battery.
1. Connect **KG645 Terminal 2 (BATT+)** through a **1A Inline Fuse** to the **VIN** terminal of the ESP8266 board.
2. Connect **KG645 Terminal 1 (BATT-)** directly to the **GND** terminal of the ESP8266 board.

### B. Simulate Mains Control Relay (GPIO4) & Transistor Driver Mod
The relay controls the digital input on the controller to suppress starting.
1. Connect the relay **COM (Common)** terminal to **KG645 Terminal 1 (BATT-)** (or common GND).
2. Connect the relay **NO (Normally Open)** terminal to **KG645 Terminal 11 (DIG_IN_B)**.

**Important Hardware Modification (Resistor Hack)**:
The onboard relay driver transistor (Q1) base resistor (R6) is 4.7kΩ on the standard HW-622 board. When driven by the ESP8266's 3.3V GPIO4, it provides only ~0.55mA base current. This is insufficient to saturate the transistor for a 70mA relay coil, resulting in a collector voltage of 1.9V and only 3.1V across the relay.
* **Solution**: Solder **three 220Ω through-hole resistors in series** (yielding **660Ω**) in parallel across the SMD resistor **R6**.
* **Result**: The equivalent resistance becomes **~578Ω**, drawing **~4.5mA** from GPIO4 (safe for ESP8266). This drops the collector voltage to **~0.1V**, causing the relay to click and latch perfectly.

### C. Generator Status Monitoring (Direct Built-in Optocoupler GPIO5)
To monitor if the generator is running without risking electrical noise or starter cranking surges frying the ESP8266:
1. Connect **KG645 Terminal 3 (OUT A)** (Generator Available, 12V active-HIGH) directly to the **IN** terminal of the `INPUT_1` 3-pin block on the board.
2. Connect **KG645 Terminal 1 (BATT-)** (GND reference) directly to the **GND_EXIT** terminal of the `INPUT_1` 3-pin block on the board.

*Note: The HW-622 board features a built-in PC817 optocoupler and a 4.7kΩ current-limiting resistor (R8) on this input line. Tapping directly to OUT A draws ~2.3mA at 12V, which safely triggers the optocoupler and pulls GPIO5 LOW without requiring external resistors or level-shifting modules.*

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
| **V2** | LED Switch | Integer | `0` to `1` | Dashboard toggle button to manually control the physical onboard blue LED (GPIO2). |
| **V3** | Auto Mode | Integer | `0` to `1` (Default: `1`) | Toggle switch to enable automatic schedule-driven mode (`1` = Auto, `0` = Manual). |
| **V4** | Simulate Mains | Integer | `0` to `1` (Default: `0`) | Controls the Night Mode relay (1 = ON/Suppressed, 0 = OFF/Normal). |
| **V5** | Dig In | Integer | `0` to `1` | Generator running status (1 = Running, 0 = Stopped). |
| **V6** | Terminal Log | String | N/A | Diagnostics and system boot/NTP sync event logs. |
| **V7** | System Status | String | N/A | Current mode status (e.g. "Night Mode Active"). |
| **V8** | WiFi RSSI | Integer | `-100` to `0` dBm | Displays local WiFi signal strength. |
| **V9** | Start Hour | Integer | `0` to `23` (Default: `23`) | Quiet Hours Start Time hour picker slider/step widget. |
| **V10** | End Hour | Integer | `0` to `23` (Default: `6`) | Quiet Hours End Time hour picker slider/step widget. |

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

### B. Boot-Time State Syncing, Auto Mode & Manual Overrides
1.  **Offline Boot**: The relay remains OFF. The generator acts in normal AMF mode.
2.  **Network Connect & Time Sync**: Once the ESP8266 connects to WiFi and syncs its local time via NTP, if Auto Mode is active, it enforces the Quiet Hours schedule (ON between the configured Start Hour and End Hour; OFF otherwise).
3.  **Blynk Connection Sync**: Once Blynk connects, the ESP8266 syncs the state of `V3` (Auto Mode), `V9` (Start Hour), and `V10` (End Hour) from the cloud.
    *   If **Auto Mode is enabled (1)**: The controller enforces the time-based schedule, and automatically updates the Simulate Mains switch (`V4`) to match.
    *   If **Auto Mode is disabled (0)**: The controller runs in Manual mode, and pulls the last saved switch position of `V4` from the Blynk cloud to set the relay.
4.  **Auto/Manual Interlocking Logic**:
    *   To return to time-based control at any time, toggle **Auto Mode (V3) ON** in the Blynk app. The relay will immediately snap to the scheduled state for the current time based on the active **Start Hour (V9)** and **End Hour (V10)**.
    *   Toggling the **Simulate Mains (V4)** switch directly in the Blynk app at any time acts as a manual override and will **automatically disable Auto Mode (setting V3 to OFF)** so the manual choice persists.

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
