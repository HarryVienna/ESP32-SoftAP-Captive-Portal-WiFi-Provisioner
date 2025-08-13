#include "include/wifi_provisioner.hpp"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <vector>
#include <algorithm>
#include <cstring>
#include <time.h>

static const char *TAG = "WIFI_PROV";
#define PROV_NVS_NAMESPACE "wifi_prov"

// Bit für unsere Event Group
#define PROV_SUCCESS_BIT BIT0

// Prototypen & eingebettete Files
void start_dns_server();
void stop_dns_server();
extern const char root_html_start[] asm("_binary_index_de_html_start");
extern const char root_html_end[]   asm("_binary_index_de_html_end");
extern const char style_css_start[] asm("_binary_style_css_start");
extern const char style_css_end[]   asm("_binary_style_css_end");

#define WIFI_MAX_RETRIES 5

/**
 * @brief Hilfsfunktion zum Dekodieren eines URL-kodierten Strings.
 * * @param out Puffer für den dekodierten String.
 * @param in  Der URL-kodierte Eingabe-String.
 */
static void url_decode(char *out, const char *in, size_t out_len) {
    char *end = out + out_len - 1; // Ende des Puffers
    while (*in && out < end) { // Prüfe Puffergrenze
        if (*in == '%') {
            if (in[1] && in[2]) {
                char hex[3] = { in[1], in[2], '\0' };
                *out++ = strtol(hex, NULL, 16);
                in += 3;
            }
        } else if (*in == '+') {
            *out++ = ' ';
            in++;
        } else {
            *out++ = *in++;
        }
    }
    *out = '\0'; // Nullterminierung sicherstellen
}

// Konstruktor: Erstellt die Event Group
WifiProvisioner::WifiProvisioner() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    _provisioning_event_group = xEventGroupCreate();
    init_wifi_();
}

// Destruktor: Gibt die Event Group frei
WifiProvisioner::~WifiProvisioner() {
    stop_web_server_();
    stop_dns_server();
    vEventGroupDelete(_provisioning_event_group);
}

// Initialisierungen
void WifiProvisioner::init_wifi_() {
    ESP_LOGI(TAG, "Initialize WiFi...");

    if (wifi_initialized_) return;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, this, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, this, NULL));

    ESP_LOGI(TAG, "Finished initializing WiFi...");

    wifi_initialized_ = true;
}

esp_err_t WifiProvisioner::start_ap_(const std::string& ssid, const std::string& password) {
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, ssid.c_str(), sizeof(wifi_config.ap.ssid) -1);

    // Konfiguration für den Access Point Teil
    if (password.length() >= 8) {
        wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        strncpy((char*)wifi_config.ap.password, password.c_str(), sizeof(wifi_config.ap.password) - 1);
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    wifi_config.ap.max_connection = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

void WifiProvisioner::stop_ap_() {
    // Stoppt das Senden von Beacon-Frames und trennt alle Clients
    ESP_ERROR_CHECK(esp_wifi_stop());
    
    // Setzt den Modus auf NULL, um den WiFi-Teil in einen Low-Power-Zustand zu versetzen
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL)); 
    ESP_LOGI(TAG, "SoftAP stopped.");
}

void WifiProvisioner::stop_web_server_() { 
    ESP_LOGI(TAG, "Stopping web server...");
    if (server_) httpd_stop(server_); 
    ESP_LOGI(TAG, "Stopping web server finished...");
}

esp_err_t WifiProvisioner::start_web_server_() {
    ESP_LOGI(TAG, "Starting web server...");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.max_uri_handlers = 10;    // Erlaube mehr URI-Handler (gute Praxis)
    config.max_req_hdr_len  = 1024;  // Der Standardwert ist oft 512 Bytes. Wir verdoppeln ihn.
    config.max_resp_headers = 10;    // Erlaube mehr Response-Header (gute Praxis)
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server_, &config) != ESP_OK) return ESP_FAIL;
    
    httpd_uri_t root_uri = { "/", HTTP_GET, root_get_handler_, this };
    httpd_uri_t style_uri = { "/style.css", HTTP_GET, style_get_handler_, this };
    httpd_uri_t scan_uri = { "/scan.json", HTTP_GET, scan_get_handler_, this };
    httpd_uri_t save_uri = { "/save", HTTP_POST, save_post_handler_, this };
    httpd_uri_t captive_uri = { "/*", HTTP_GET, captive_portal_handler_, this };
    
    httpd_register_uri_handler(server_, &root_uri);
    httpd_register_uri_handler(server_, &style_uri);
    httpd_register_uri_handler(server_, &scan_uri);
    httpd_register_uri_handler(server_, &save_uri);
    httpd_register_uri_handler(server_, &captive_uri);

    ESP_LOGI(TAG, "Starting web server finished...");

    return ESP_OK;
}

// Öffentliche Methoden
esp_err_t WifiProvisioner::start_provisioning(const std::string& ap_ssid, bool persistent_storage, const std::string& ap_password) {
    _persistent_storage = persistent_storage;

    ESP_LOGI(TAG, "Starting provisioning mode...");
    ESP_ERROR_CHECK(start_ap_(ap_ssid, ap_password));
    start_dns_server();
    ESP_ERROR_CHECK(start_web_server_());

    ESP_LOGI(TAG, "Provisioning running. Waiting for user to submit credentials...");
    
    // Warte hier, bis der save_post_handler das PROV_SUCCESS_BIT setzt
    xEventGroupWaitBits(_provisioning_event_group, PROV_SUCCESS_BIT,
                        pdTRUE, // Bit nach dem Warten löschen
                        pdFALSE,
                        portMAX_DELAY);

    ESP_LOGI(TAG, "Credentials received. Shutting down provisioning services.");
    
    // Aufräumen: Server und AP stoppen
    stop_dns_server();
    stop_web_server_();
    stop_ap_();

    return ESP_OK;
}

bool WifiProvisioner::is_provisioned() {
    nvs_handle_t h;
    if(nvs_open(PROV_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t s=0;
    bool k = nvs_get_str(h, "ssid", NULL, &s) == ESP_OK && s > 1;
    nvs_close(h);
    return k;
}

esp_err_t WifiProvisioner::get_credentials() {
    ESP_LOGI(TAG, "Loading credentials from NVS into class...");
    return load_credentials_from_nvs_(_ssid, _password, _timezone);
}

esp_err_t WifiProvisioner::connect_sta(const char* hostname) {
    // 1. Sicherheitsprüfung: Sind überhaupt Zugangsdaten in der Klasse vorhanden?
    if (_ssid.empty()) {
        ESP_LOGE(TAG, "Cannot connect: No credentials loaded. Call 'load_credentials_from_nvs()' or 'start_provisioning()' first.");
        return ESP_FAIL;
    }

    // 2. Logging der in der Klasse gespeicherten Daten
    ESP_LOGI(TAG, "Attempting to connect with credentials stored in the class instance:");
    ESP_LOGI(TAG, "  -> SSID:     '%s'", _ssid.c_str());
    ESP_LOGI(TAG, "  -> Password: %s", _password.length() > 0 ? "YES (hidden for security)" : "NO (open network)");
    ESP_LOGI(TAG, "  -> Timezone: '%s'", _timezone.c_str());

    // 3. Hostname für das STA-Interface setzen
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, hostname));
        ESP_LOGI(TAG, "  -> Hostname set to: '%s'", hostname);
    }

    // 4. WiFi-Konfiguration mit den Member-Variablen erstellen
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, _ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, _password.c_str(), sizeof(wifi_config.sta.password) - 1);
    
    if (_password.length() > 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    
    // 5. WiFi-System starten (der eigentliche Verbindungsaufbau geschieht im Event-Handler)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi system started. Waiting for connection events...");

    // 6. Zeitzone aus der Member-Variable anwenden
    setenv("TZ", _timezone.c_str(), 1);
    tzset();
    ESP_LOGI(TAG, "System timezone set to: '%s'", _timezone.c_str());

    return ESP_OK;
}

// NVS Handler
esp_err_t WifiProvisioner::load_credentials_from_nvs_(std::string& ssid, std::string& password, std::string& timezone) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(PROV_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t required_size;
    // SSID
    if (nvs_get_str(h, "ssid", NULL, &required_size) == ESP_OK) {
        ssid.resize(required_size);
        nvs_get_str(h, "ssid", &ssid[0], &required_size); // &ssid[0] gibt ab C++11 einen schreibbaren Pointer auf den internen Puffer des Strings zurück
        ssid.pop_back(); // NVS speichert mit \0, das entfernen wir aus dem std::string
    }
    // Password
    if (nvs_get_str(h, "password", NULL, &required_size) == ESP_OK) {
        password.resize(required_size);
        nvs_get_str(h, "password", &password[0], &required_size);
        password.pop_back();
    }
    // Timezone
    if (nvs_get_str(h, "timezone", NULL, &required_size) == ESP_OK) {
        timezone.resize(required_size);
        nvs_get_str(h, "timezone", &timezone[0], &required_size);
        timezone.pop_back();
    }
    
    nvs_close(h);
    return ESP_OK;
}

esp_err_t WifiProvisioner::save_credentials_to_nvs_() {
    nvs_handle_t nvs_handle;
    ESP_LOGI(TAG, "Opening NVS to save credentials...");
    esp_err_t err = nvs_open(PROV_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    // Schreibe die Werte aus den Member-Variablen in den NVS
    err = nvs_set_str(nvs_handle, "ssid", _ssid.c_str());
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to save ssid to NVS");

    err = nvs_set_str(nvs_handle, "password", _password.c_str());
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to save password to NVS");

    err = nvs_set_str(nvs_handle, "timezone", _timezone.c_str());
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to save timezone to NVS");
    
    // Bestätige die Schreibvorgänge
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit credentials to NVS");
    } else {
        ESP_LOGI(TAG, "Credentials successfully committed to NVS.");
    }
    
    // Schließe den NVS-Handle
    nvs_close(nvs_handle);
    return err;
}

// HTTP Handler
esp_err_t WifiProvisioner::root_get_handler_(httpd_req_t *r) { 
    httpd_resp_set_type(r, "text/html"); 
    return httpd_resp_send(r, root_html_start, root_html_end - root_html_start); 
}
esp_err_t WifiProvisioner::style_get_handler_(httpd_req_t *r) { 
    httpd_resp_set_type(r, "text/css"); 
    return httpd_resp_send(r, style_css_start, style_css_end - style_css_start); 
}
esp_err_t WifiProvisioner::captive_portal_handler_(httpd_req_t *r) { 
    httpd_resp_set_status(r, "302 Found"); 
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1"); 
    return httpd_resp_send(r, NULL, 0); 
}

esp_err_t WifiProvisioner::scan_get_handler_(httpd_req_t *req) {
    ESP_LOGI(TAG, "==> /scan.json Handler wurde vom Client aufgerufen.");

    // 1. WLAN-Scan durchführen
    uint16_t num_aps = 0;
    // esp_wifi_scan_stop(); // Stoppt einen evtl. laufenden Scan
    esp_wifi_scan_start(NULL, true); // true = blockierend, wartet auf das Ergebnis
    esp_wifi_scan_get_ap_num(&num_aps);
    
    ESP_LOGI(TAG, "==> Scan beendet. Gefundene Netzwerke: %u", num_aps);

    if (num_aps == 0) {
        ESP_LOGW(TAG, "Keine Netzwerke gefunden. Sende leere JSON-Liste.");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"aps\":[]}", HTTPD_RESP_USE_STRLEN);
    }
    
    std::vector<wifi_ap_record_t> ap_records(num_aps);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&num_aps, ap_records.data()));

    // 2. Liste alphabetisch sortieren
    std::sort(ap_records.begin(), ap_records.end(), [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return strcmp((const char*)a.ssid, (const char*)b.ssid) < 0;
    });

    // 3. JSON aus der sortierten Liste erstellen
    cJSON *root = cJSON_CreateObject();
    cJSON *aps = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "aps", aps);

    for (const auto& record : ap_records) {
        cJSON *ap_item = cJSON_CreateObject();
        cJSON_AddStringToObject(ap_item, "ssid", (const char *)record.ssid);
        cJSON_AddNumberToObject(ap_item, "rssi", record.rssi);
        cJSON_AddItemToArray(aps, ap_item);
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "==> Sende JSON-Antwort an den Browser: %s", json_str);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    // Speicher freigeben
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}


esp_err_t WifiProvisioner::save_post_handler_(httpd_req_t *req) {
    // Puffer zum Lesen der POST-Daten erstellen
    char buf[256];
    int ret, remaining = req->content_len;
    std::string content;

    // Lese den gesamten Body der POST-Anfrage
    while (remaining > 0) {
        ret = httpd_req_recv(req, buf, std::min(remaining, (int)sizeof(buf)));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "Failed to receive POST data");
            return ESP_FAIL;
        }
        content.append(buf, ret);
        remaining -= ret;
    }

    // Variablen für die extrahierten Werte
    char ssid_encoded[128] = {0};
    char ssid_decoded[128] = {0};
    char password_encoded[64] = {0};
    char password_decoded[64] = {0};
    char timezone_encoded[128] = {0};
    char timezone_decoded[128] = {0};

    // Extrahiere die Key-Value-Paare aus dem POST-Body
    if (httpd_query_key_value(content.c_str(), "ssid", ssid_encoded, sizeof(ssid_encoded)) != ESP_OK ||
        httpd_query_key_value(content.c_str(), "timezone", timezone_encoded, sizeof(timezone_encoded)) != ESP_OK ||
        strlen(ssid_encoded) == 0 || strlen(timezone_encoded) == 0) {
        
        ESP_LOGE(TAG, "Bad request: ssid or timezone parameter missing.");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "BAD REQUEST: SSID und Zeitzone sind erforderlich.");
        return ESP_FAIL;
    }

    // Passwort ist optional
    httpd_query_key_value(content.c_str(), "password", password_encoded, sizeof(password_encoded));

    // Hole den `this` Pointer auf die Klasseninstanz
    auto* provisioner = static_cast<WifiProvisioner*>(req->user_ctx);

    url_decode(ssid_decoded,   ssid_encoded,   sizeof(ssid_decoded));
    url_decode(password_decoded, password_encoded, sizeof(password_decoded));
    url_decode(timezone_decoded, timezone_encoded, sizeof(timezone_decoded));

    // Speichere die empfangenen und dekodierten Daten in den Member-Variablen der Klasse
    provisioner->_ssid = ssid_decoded;
    provisioner->_password = password_decoded;
    provisioner->_timezone = timezone_decoded;

    ESP_LOGI(TAG, "Credentials temporarily stored. Decoded timezone: %s", timezone_decoded);

    // Wenn das `persistent_storage`-Flag gesetzt wurde, speichere die Daten auch dauerhaft im NVS
    if (provisioner->_persistent_storage) {
        if (provisioner->save_credentials_to_nvs_() == ESP_OK) {
            ESP_LOGI(TAG, "Credentials also saved persistently to NVS.");
        } else {
            ESP_LOGE(TAG, "Failed to save credentials to NVS!");
        }
    }
    
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);

    // Signalisiere der wartenden start_provisioning-Funktion, dass die Eingabe erfolgreich war
    xEventGroupSetBits(provisioner->_provisioning_event_group, PROV_SUCCESS_BIT);

    return ESP_OK;
}

void WifiProvisioner::wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    // Hole die Instanz der Klasse aus dem Argument-Pointer
    WifiProvisioner* provisioner = static_cast<WifiProvisioner*>(arg);
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "EVENT: STA_START received. Initiating connection...");
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG, "EVENT: STA_DISCONNECTED. Reason code: %d.", event->reason);

        if (provisioner->_retry_num  < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            provisioner->_retry_num ++;
            ESP_LOGI(TAG, "Retrying to connect... (Attempt %d/%d)", provisioner->_retry_num , WIFI_MAX_RETRIES);
        } else {
            ESP_LOGE(TAG, "Failed to connect after %d attempts. Erasing credentials and rebooting into provisioning mode.", WIFI_MAX_RETRIES);
            
            // Lösche die gespeicherten Zugangsdaten
            nvs_handle_t nvs_handle;
            nvs_open(PROV_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
            nvs_erase_key(nvs_handle, "ssid");
            nvs_erase_key(nvs_handle, "password");
            nvs_erase_key(nvs_handle, "timezone");
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            
            // Starte das Gerät neu
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "EVENT: GOT_IP. Successfully connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        // Setze den Zähler bei Erfolg zurück
        provisioner->_retry_num = 0; 

        // Starte die Zeitsynchronisierung
        provisioner->synchronize_time();
    }
}

void WifiProvisioner::time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "Zeitsynchronisierung erfolgreich abgeschlossen.");
}

void WifiProvisioner::synchronize_time() {
    if (_sntp_initialized) {
        ESP_LOGI(TAG, "SNTP is already initialized. Skipping.");
        return; // Breche die Funktion hier ab
    }

    ESP_LOGI(TAG, "Initialisiere SNTP-Zeitsynchronisierung...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    _sntp_initialized = true; // Setze das Flag nach der ersten erfolgreichen Initialisierung
}