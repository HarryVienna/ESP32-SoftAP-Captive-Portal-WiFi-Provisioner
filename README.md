# ESP-IDF WiFi Provisioning with Captive Portal

A robust, self-contained C++ component for the ESP32 that provides a user-friendly WiFi provisioning experience using a captive portal. 
This project is built using the pure **Espressif IoT Development Framework (ESP-IDF)**, with no Arduino dependencies.

The goal is to provide a simple component for any IoT project that requires initial WiFi setup by an end-user.

_screenshot_

## Features

- **User-Friendly Captive Portal:** Automatically opens a configuration page on a user's phone or laptop after connecting to the ESP32's access point.
- **Dynamic WiFi Scanning:** Scans for and lists available WiFi networks in a dropdown menu.
- **Secure Password Entry:** Includes a "Show/Hide" button for the password field to prevent typos.
- **Advanced Timezone Selection:** Uses chained dropdowns (Region -> Timezone) for a clean user interface.
- **Robust Error Handling:** If a user saves incorrect credentials (e.g., wrong password), the device will attempt to connect a few times, then automatically erase the bad credentials and restart in provisioning mode. This makes the device "unbrickable" by a user.
- **Optional Persistent Storage:** The provisioning process can be configured to save credentials permanently to NVS flash or to use them only for the current session (stored in RAM).
- **Automatic Time Sync (SNTP):** Once connected to WiFi, the class automatically synchronizes the system time with an internet time server.
- **Custom Hostname:** Sets a user-defined hostname for the device on the local network.
- **Fully Encapsulated:** The class manages all its own dependencies (NVS, WiFi, and event system initialization) safely, keeping your app_main clean and simple.

## How it Works: The Captive Portal

The "magic" of the automatically opening configuration page is achieved with a classic captive portal mechanism.

1. **Access Point:** The ESP32 creates an open WiFi network (e.g., "ESP32-Setup").
2. **DHCP & DNS:** When a user connects, the ESP32 assigns them an IP address and, crucially, tells them: "For all internet name lookups (DNS), you must ask me."
3. **DNS Hijack:** The user's device immediately tries to check for internet by accessing a known address (e.g., google.com). It asks the ESP32 for the IP. The DNS server on the ESP32 intercepts this request and always replies with a lie: "The IP address you're looking for is my own, 192.168.4.1."
4. **Web Server Redirect:** The device's browser, believing the lie, sends a request to the ESP32's IP. The web server on the ESP32 serves the index.html configuration page.
5. **OS Detection:** The phone's operating system detects that it received a configuration page instead of the simple success message it expected. It concludes the network requires a login and automatically presents the page to the user.

## Project Structure

The core logic is contained within the wifi_provisioner component, making it highly portable to other ESP-IDF projects.

```
esp-idf-project/
├── main/
│ ├── main.cpp
│ └── CMakeLists.txt
├── components/
│ └── wifi_provisioner/ <-- All provisioning logic is here
│ ├── web/
│ │ ├── index.html
│ │ └── style.css
│ ├── include/
│ │ └── wifi_provisioner.hpp
│ ├── dns_server.cpp
│ ├── wifi_provisioner.cpp
│ └── CMakeLists.txt
└── ...
```

## How to Use

1. **Add the Component:** Copy the entire wifi_provisioner folder into the components directory of your ESP-IDF project.
2. **Update main/CMakeLists.txt:** Make sure your main application depends on the component.
```   
# main/CMakeLists.txt
idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES wifi_provisioner)
```
3. **Use the Class** in main.cpp: The class is designed to be extremely simple to use.
```
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "wifi_provisioner.hpp" // The only header you need for WiFi setup

static const char* TAG = "MAIN_APP";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Application starting...");

    // Create the provisioner object. The constructor handles all one-time initializations.
    WifiProvisioner provisioner;

    // Check if credentials are already saved in NVS
    if (provisioner.is_provisioned()) {
        // If yes, load them into the class
        provisioner.load_credentials_from_nvs();
    } else {
        // If no, start the blocking provisioning process.
        // The `true` flag means credentials will be saved permanently.
        provisioner.start_provisioning("ESP32-Setup", true);
    }

    // Now, connect to WiFi using either the loaded or the newly entered credentials.
    // The time will be synced automatically upon connection.
    provisioner.connect_sta("My-ESP32-Device");

    ESP_LOGI(TAG, "Main application logic can now run.");
    while(true) {
        // Your main application code goes here...
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
```

## Configuration

The web interface files (index.html, style.css) are embedded directly into the firmware binary. This is configured in components/wifi_provisioner/CMakeLists.txt:
```
# ...
target_add_binary_data(${COMPONENT_TARGET} "web/index.html" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "web/style.css"  TEXT)
```
