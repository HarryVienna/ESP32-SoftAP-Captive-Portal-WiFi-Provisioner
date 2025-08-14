/**
 * @file wifi_provisioner.hpp
 * @brief Eine robuste, vollständig gekapselte Klasse für die WLAN-Provisionierung auf dem ESP32.
 *
 * ## Konzept: WiFi-Provisioning
 *
 * IoT-Geräte wie der ESP32 benötigen in der Regel eine WLAN-Verbindung, um nützlich zu sein. Die Zugangsdaten
 * (SSID und Passwort) im Code fest zu hinterlegen, ist unflexibel und unsicher. Der Prozess, einem Gerät
 * diese Informationen bei der Ersteinrichtung auf benutzerfreundliche Weise mitzuteilen, wird als
 * **Provisioning** bezeichnet.
 *
 * Diese Klasse implementiert die gängigste und benutzerfreundlichste Methode für Geräte ohne Bildschirm
 * oder Tastatur: den **Soft AP (Access Point) + Captive Portal** Modus.
 *
 * ## Funktionsweise: Das Captive Portal
 *
 * Das Herzstück der Klasse ist ein Mechanismus, der das Smartphone eines Benutzers nach der Verbindung
 * mit dem ESP32 automatisch auf eine Konfigurationsseite "zwingt". Dies ist dieselbe Technologie,
 * die auch in Hotels oder Flughäfen zum Einsatz kommt.
 *
 * Stellen Sie sich den ESP32 wie einen cleveren Hotel-Portier vor:
 *
 * 1.  **Verbindung & DHCP:** Ein Gast (das Smartphone) betritt das Hotel (verbindet sich mit dem vom ESP32
 * erstellten WLAN). Der Portier (DHCP-Server des ESP32) gibt dem Gast einen Zimmerschlüssel (IP-Adresse)
 * und sagt: "Wenn Sie nach dem Weg fragen wollen (DNS-Anfrage), fragen Sie ausschließlich mich."
 *
 * 2.  **Automatischer Konnektivitäts-Check:** Das Smartphone möchte sofort wissen, ob es einen Weg nach
 * draußen (ins Internet) gibt. Es ruft dafür automatisch eine bekannte Adresse an (z.B. "google.com").
 *
 * 3.  **DNS-Entführung (Der Trick):** Das Smartphone fragt den Portier (DNS-Server des ESP32): "Wo ist der
 * Ausgang zu 'google.com'?" Der Portier lügt absichtlich und antwortet immer mit derselben
 * Information: "Der Ausgang ist direkt hier an der Rezeption" (er antwortet mit seiner eigenen
 * IP-Adresse `192.168.4.1`).
 *
 * 4.  **Webserver-Antwort:** Das Smartphone geht zur Rezeption (sendet eine HTTP-Anfrage an `192.168.4.1`).
 * Dort drückt ihm der Portier (Webserver des ESP32) anstelle einer Wegbeschreibung das Anmeldeformular
 * des Hotels in die Hand (die `index.html`-Konfigurationsseite).
 *
 * 5.  **Captive Portal Erkennung:** Das Betriebssystem des Smartphones erkennt die List. Es merkt: "Das ist
 * nicht der Ausgang, das ist eine Anmelde- oder Konfigurationsseite." Daraufhin öffnet es diese
 * Seite prominent in einem speziellen Browser-Fenster für den Benutzer.
 *
 * ## Besondere Merkmale der Klasse
 *
 * Diese Klasse wurde im Laufe der Entwicklung um mehrere robuste Funktionen erweitert:
 *
 * - **Vollständige Kapselung:** Die Klasse kümmert sich um alle notwendigen System-Initialisierungen
 * (NVS, WiFi, Events etc.) auf eine sichere Weise, die nur einmal ausgeführt wird. Die `app_main`-Funktion
 * bleibt dadurch extrem sauber und einfach.
 *
 * - **Blockierender Prozess ohne Neustarts:** Der `start_provisioning()`-Prozess ist "blockierend".
 * Das bedeutet, die `app_main`-Funktion wartet, bis der Benutzer seine Daten eingegeben hat.
 * Danach wird die Verbindung direkt hergestellt, ohne dass ein störender Neustart des Geräts nötig ist.
 *
 * - **Optionale dauerhafte Speicherung:** Die `start_provisioning()`-Methode hat ein Flag `persistent_storage`.
 * Ist dieses `true`, werden die Daten im NVS-Flash gespeichert. Ist es `false`, werden die Daten nur
 * temporär im RAM gehalten und sind nach einem Neustart verloren. Um Konflikte zu vermeiden, wird
 * dem ESP-IDF WiFi-Treiber das automatische Speichern von Zugangsdaten explizit untersagt (`WIFI_STORAGE_RAM`).
 *
 * - **Automatische Fehlerbehandlung:** Gibt ein Benutzer ein falsches Passwort ein, würde das Gerät
 * normalerweise in einer Endlosschleife versuchen, sich zu verbinden. Diese Klasse zählt die
 * Fehlversuche. Nach 5 erfolglosen Versuchen löscht sie die fehlerhaften Zugangsdaten aus dem NVS
 * und startet neu, wodurch das Gerät automatisch wieder im Konfigurationsmodus ist. Es ist für den
 * Benutzer somit unmöglich, das Gerät "versehentlich zu bricken".
 *
 * ## Verwendung in der Anwendung (`main.cpp`)
 *
 * Die Verwendung der Klasse ist denkbar einfach und erfordert keine Kenntnisse über die internen Abläufe.
 *
 * ```cpp
 * #include "freertos/FreeRTOS.h"
 * #include "freertos/task.h"
 * #include "esp_log.h"
 * #include "wifi_provisioner.hpp" // Der einzige Header, der benötigt wird.
 *
 * static const char* TAG = "MAIN_APP";
 *
 * extern "C" void app_main(void) {
 * ESP_LOGI(TAG, "Application starting...");
 *
 * // Erstelle das Objekt. Der Konstruktor kümmert sich um ALLES.
 * WifiProvisioner provisioner;
 *
 * // Prüfe, ob das Gerät bereits konfiguriert ist.
 * if (provisioner.is_provisioned()) {
 * // Wenn ja, lade die Daten aus dem NVS in das Objekt.
 * provisioner.get_credentials();
 * } else {
 * // Wenn nein, starte den blockierenden Provisionierungs-Prozess.
 * // 'true' bedeutet, dass die eingegebenen Daten dauerhaft gespeichert werden.
 * provisioner.start_provisioning("ESP32-Setup", true);
 * }
 * * // Stelle nun die Verbindung her, entweder mit den alten (geladenen)
 * // oder den neuen (frisch eingegebenen) Zugangsdaten.
 * provisioner.connect_sta("Mein-ESP32");
 *
 * ESP_LOGI(TAG, "Main application logic can now run.");
 * while(true) {
 * // Hier läuft Ihre eigentliche Anwendungslogik...
 * vTaskDelay(pdMS_TO_TICKS(10000));
 * }
 * }
 * ```
 */

#pragma once
#include <string>
#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h" 
#include "esp_sntp.h"

class WifiProvisioner {
public:
    WifiProvisioner();
    ~WifiProvisioner();

    /**
     * @brief Startet den blockierenden Provisionierungs-Modus.
     * * Startet AP, DNS- und Webserver und wartet, bis der Benutzer
     * im Captive Portal seine Daten eingegeben hat. Die Server werden danach wieder beendet.
     *
     * @param ap_ssid Der Name des WLAN-Netzwerks, das der ESP32 aufspannt.
     * @param persistent_storage Wenn true, werden die Daten dauerhaft im NVS gespeichert.
     * Wenn false, werden sie nur temporär im Speicher gehalten.
     * @param ap_password Optionales Passwort für den Access Point.
     * @return esp_err_t ESP_OK bei Erfolg.
     */
    esp_err_t start_provisioning(const std::string& ap_ssid, bool persistent_storage, const std::string& ap_password = "");

    /**
     * @brief Prüft, ob gültige WLAN-Zugangsdaten dauerhaft im NVS gespeichert sind.
     */
    bool is_provisioned();

    /**
     * @brief Lädt dauerhaft gespeicherte Zugangsdaten aus dem NVS in die Klasse.
     * @return esp_err_t ESP_OK bei Erfolg.
     */
    esp_err_t get_credentials();

    /**
     * @brief Versucht, eine Verbindung mit den in der Klasse gespeicherten Zugangsdaten herzustellen.
     *
     * @param hostname Der gewünschte Name des Geräts im Netzwerk.
     * @return esp_err_t ESP_OK bei Erfolg der Initiierung.
     */
    esp_err_t connect_sta(const char* hostname);

    /**
     * @brief Konfiguriert und startet den SNTP-Client zur Zeitsynchronisierung.
     * * @note Wird idealerweise automatisch aufgerufen, sobald eine WLAN-Verbindung steht.
     */
    void synchronize_time();

private:
    void init_wifi_();

    esp_err_t start_ap_(const std::string& ssid, const std::string& password);
    void stop_ap_(); 

    esp_err_t start_web_server_();
    void stop_web_server_();

    static esp_err_t root_get_handler_(httpd_req_t *req);
    static esp_err_t scan_get_handler_(httpd_req_t *req);
    static esp_err_t save_post_handler_(httpd_req_t *req);
    static esp_err_t style_get_handler_(httpd_req_t *req);
    static esp_err_t captive_portal_handler_(httpd_req_t *req);

    esp_err_t load_credentials_from_nvs_(std::string& ssid, std::string& password, std::string& timezone);
    esp_err_t save_credentials_to_nvs_();

    // Statische Methoden für C-Callbacks
    static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void time_sync_notification_cb(struct timeval *tv);

    // Member-Variablen zum Speichern der Zugangsdaten
    std::string _ssid;
    std::string _password;
    std::string _timezone;

    // Anzahl der Connect Versuche
    int _retry_num = 0;

    // Flag, um die SNTP-Initialisierung zu verfolgen
    bool _sntp_initialized = false; 

    // Konfigurations-Flags
    bool _persistent_storage = false;
    
    // FreeRTOS-Objekte für die Synchronisation
    EventGroupHandle_t _provisioning_event_group;
    
    httpd_handle_t server_ = nullptr;
    bool wifi_initialized_ = false;
};