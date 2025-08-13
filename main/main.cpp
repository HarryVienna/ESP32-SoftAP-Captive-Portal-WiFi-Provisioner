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
        time_t now;
        struct tm timeinfo;
        char strftime_buf[64];

        // Hole die aktuelle Zeit
        time(&now);
        
        // Konvertiere sie in die lokale Zeit (unter Berücksichtigung der gesetzten Zeitzone)
        localtime_r(&now, &timeinfo);

        // Prüfe, ob die Zeit schon synchronisiert wurde.
        // (tm_year ist Jahre seit 1900, also ist alles < 100 vor dem Jahr 2000)
        if (timeinfo.tm_year < 100) {
            ESP_LOGI(TAG, "Zeit ist noch nicht mit dem NTP-Server synchronisiert.");
        } else {
            // Formatiere die Zeit in einen lesbaren String (z.B. "Mittwoch, 13. August 2025 10:32:29")
            // strftime-Referenz: https://www.cplusplus.com/reference/ctime/strftime/
            strftime(strftime_buf, sizeof(strftime_buf), "%A, %d. %B %Y %H:%M:%S", &timeinfo);
            
            // Gib die formatierte Zeit aus
            ESP_LOGI(TAG, "Aktuelle lokale Zeit: %s", strftime_buf);
        }
        
        // Warte eine Sekunde bis zur nächsten Ausgabe
        vTaskDelay(pdMS_TO_TICKS(1000));
    } 
}