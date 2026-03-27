# ESP8266 Adaptive Leak Detection System (Leak Monitor Pro)

An advanced, WiFi-enabled water flow monitoring and adaptive leak detection system built around the ESP8266 microcontroller. This project utilizes two flow sensors to measure the inlet and outlet flow rates, establishes a baseline dynamically, and halts the water pump autonomously if the ratio indicates a potential leak. It includes a built-in web dashboard, an LCD, an audible buzzer, and integrates directly with Telegram to send instant leak alerts and CSV data logs.

## 🌟 Key Features

*   **Adaptive Leak Detection:** Dynamically learns the baseline flow ratio between the inlet and outlet sensors during a "Learning Phase," meaning it adapts to different pipe constraints and sensor variations automatically.
*   **3-Phase Operation:**
    *   **Phase 1 (Filling):** Lets water run through initially without calculating ratios to bleed air from the pipes.
    *   **Phase 2 (Learning):** Calculates the average flow ratio between the two sensors.
    *   **Phase 3 (Monitoring):** Actively monitors for deviations; if the ratio deviates beyond a set margin (default 25%), it triggers a leak state.
*   **Automated Shut-off:** Shuts down the water pump automatically via a relay the moment a leak is confirmed.
*   **Telegram Integration:** Sends real-time bot alerts with water consumption metrics, time, and data logs.
*   **Local Web Dashboard:** Host a beautiful, animated local dashboard showing real-time metrics, ratios, charts (Chart.js), and system states.
*   **Auto-Restart Protocol:** The system waits a designated cooldown period after stopping a leak, and automatically restarts to test if the issue has resolved.

---

## 🛠️ Hardware Requirements

*   **1x ESP8266 Board** (e.g., NodeMCU v3, Wemos D1 Mini)
*   **2x Water Flow Sensors** (e.g., YF-S201 or similar hallway effect sensors)
*   **1x Relay Module** (For toggling the water pump)
*   **1x 16x2 I2C LCD Display**
*   **1x Active Buzzer**
*   **1x Water Pump** (Rated appropriately for your relay)
*   **Jumper wires & Power supply**

## 🔌 Pin Configuration

| Component | ESP8266 Pin (NodeMCU) |
| :--- | :--- |
| **Relay Module** | `D5` |
| **Inlet Flow Sensor (1)**| `D6` |
| **Outlet Flow Sensor (2)**| `D7` |
| **Buzzer** | `D3` |
| **I2C LCD SDA** | `D2` |
| **I2C LCD SCL** | `D1` |

*(Flow Sensor data pins may also require pull-up resistors, though `INPUT_PULLUP` is enabled in the software).*

---

## 💻 Software Dependencies

Ensure you install the following libraries inside the Arduino IDE Library Manager before uploading the code:
*   `ESP8266WiFi` (Built into ESP8266 board package)
*   `ESP8266WebServer` (Built into ESP8266 board package)
*   `Wire`
*   `LiquidCrystal_I2C` (Or `LCD_I2C` as used in the sketch)
*   `WiFiClientSecure` (For HTTPS Telegram communication)

---

## 🚀 Setup & Installation

1.  **Clone this Repository**
    ```bash
    git clone https://github.com/Vikramreddy17/ESP8266-Adaptive-Leak-Detection.git
    cd ESP8266-Adaptive-Leak-Detection
    ```
2.  **Open in Arduino IDE:** Open the `.ino` file in the Arduino IDE.
3.  **Configure Network Settings:**
    Find the `WIFI SETTINGS` section and update it with your network credentials:
    ```cpp
    const char* sta_ssid     = "Your_WiFi_SSID";
    const char* sta_password = "Your_WiFi_Password";
    ```
4.  **Configure Telegram Bot Integration:**
    To receive alerts, you need a Telegram Bot and your Chat ID. You can get a token by messaging `@BotFather` on Telegram. Find your Chat ID via `@userinfobot`.
    ```cpp
    const char* telegramToken  = "YOUR_BOT_TOKEN_HERE";
    const char* telegramChatID = "YOUR_CHAT_ID_HERE";
    ```
5.  **Adjust Sensitivities (Optional):**
    By default, the `pulsesPerLiter1` and `pulsesPerLiter2` are set to `2290.0` and `5870.0`. Change these according to the datasheet of your specific flow sensors.
    You can also modify the `leakMarginPercent` (default is `0.25` / 25%).
6.  **Upload to ESP8266:** Select your specific ESP8266 board and Port from the Tools menu and click Upload.

---

## 🌐 Using the Web Dashboard

1. Once the ESP8266 connects to WiFi, its assigned IP address will be displayed on the LCD and Serial Monitor.
2. Open a device connected to the same network and type that IP address into your web browser.
3. You will see the **Leak Monitor Pro** interface where you can:
    *   View real-time flow graphs
    *   Monitor Ratio vs. Baseline
    *   Control the pump manually
    *   Download historical data logs as CSV
    *   Send a test Telegram alert

---

## 📜 License & Credits

Designed by Vikram Reddy. Enjoy peace of mind and prevent water damage!
