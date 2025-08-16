#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <time.h> 
#include "wifi_provisioner.hpp"

static const char* TAG = "MAIN_APP";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Application starting...");

    WifiProvisioner provisioner;

    // SCHRITT 1: Entscheiden, woher die Daten kommen
    if (provisioner.is_provisioned()) {
        // Option A: Daten sind im NVS. Lade sie in die Klasse.
        provisioner.get_credentials();
    } else {
        // Option B: Keine Daten im NVS. Starte den Provisionierungs-Prozess.
        // Dieser füllt die Member-Variablen und speichert die Daten dauerhaft (wegen `true`).
        provisioner.start_provisioning("ESP32-Setup", false);
    }

    // SCHRITT 2: Verbinden
    // `connect_sta` verwendet nun die Daten, die in Schritt 1 geladen wurden.
    provisioner.connect_sta("Mein-ESP32");

    // Endlosschleife für die Hauptanwendung
    ESP_LOGI(TAG, "Main application logic can now run. Waiting for WiFi events...");
    while(true) {
        // Frage die Klasse direkt, ob die Zeit synchron ist.
        if (provisioner.is_time_synchronized()) {
            time_t now;
            struct tm timeinfo;
            char strftime_buf[64];

            time(&now);
            localtime_r(&now, &timeinfo);
            strftime(strftime_buf, sizeof(strftime_buf), "%A, %d. %B %Y %H:%M:%S", &timeinfo);
            
            ESP_LOGI(TAG, "Aktuelle lokale Zeit: %s", strftime_buf);
        } else {
            ESP_LOGI(TAG, "Zeit ist noch nicht mit dem NTP-Server synchronisiert.");
        }
        
        // Warte eine Sekunde bis zur nächsten Ausgabe
        vTaskDelay(pdMS_TO_TICKS(1000));
    } 
}