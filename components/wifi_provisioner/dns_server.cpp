#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "esp_netif.h"

// Standard-Port für DNS
#define DNS_PORT 53
#define DNS_MAX_LEN 256

// DNS-Header-Flags
#define DNS_FLAG_QR (1 << 7)
#define DNS_FLAG_RA (1 << 15)

#define DNS_ANSWER_LEN 16
#define DNS_HEADER_SIZE 12
#define DNS_FLAGS_OFFSET 2
#define DNS_ANSWER_COUNT_OFFSET 7
#define DNS_QUESTION_SUFFIX_SIZE 4 // QTYPE (2 bytes) + QCLASS (2 bytes)

// Answer-Sektion
#define DNS_COMPRESSION_POINTER 0xC0
#define DNS_COMPRESSION_OFFSET 0x0C
#define DNS_TYPE_A 1
#define DNS_CLASS_IN 1

//  Zeit- und Längenangaben (TTL & RDLENGTH)
#define DNS_ANSWER_TTL_S 120 // Time-To-Live in Sekunden
#define DNS_RDLENGTH_IPV4 4

// Statische Variablen für den Task
static const char *TAG = "DNS_SERVER";
static int sock_fd = -1;
static volatile bool dns_server_running = false;

/**
 * @brief Erstellt eine DNS-Antwort, die auf die IP des APs verweist.
 * @param request Der ursprüngliche DNS-Request-Packet.
 * @param response Der Puffer für das DNS-Response-Packet.
 * @param ap_ip Die IP-Adresse des Access Points.
 */
static void create_dns_response(uint8_t *request, uint8_t *response, const esp_ip4_addr_t *ap_ip) {
    // Finde das Ende der "Question"-Sektion
    uint8_t *p = request + DNS_HEADER_SIZE;
    while (*p != 0) {
        p += (*p + 1);
    }
    size_t question_len = (p - (request + DNS_HEADER_SIZE)) + 1; // Länge des Domainnamens inkl. Null-Byte

    // Kopiere Header und die komplette Question-Sektion (Name + Suffix)
    size_t request_len = DNS_HEADER_SIZE + question_len + DNS_QUESTION_SUFFIX_SIZE;
    memcpy(response, request, request_len); // Header + Question + QTYPE/QCLASS

    // Setze die Flags im Header für eine Antwort
    response[DNS_FLAGS_OFFSET] |= DNS_FLAG_QR; // Query Response
    response[DNS_FLAGS_OFFSET ] |= DNS_FLAG_RA; // Recursion Available

    // Setze die Anzahl der Antworten auf 1
    response[DNS_ANSWER_COUNT_OFFSET] = 1;

    // Setze den Pointer an das Ende der kopierten "Question"-Sektion im Response-Puffer
    p = response + request_len;

    // Füge die "Answer"-Sektion hinzu
    *p++ = DNS_COMPRESSION_POINTER;
    *p++ = DNS_COMPRESSION_OFFSET;

    *p++ = 0x00; *p++ = DNS_TYPE_A;      // Type
    *p++ = 0x00; *p++ = DNS_CLASS_IN;    // Class

    // TTL (Time To Live) in Big-Endian (Netzwerk-Byte-Reihenfolge)
    *p++ = (DNS_ANSWER_TTL_S >> 24) & 0xFF;
    *p++ = (DNS_ANSWER_TTL_S >> 16) & 0xFF;
    *p++ = (DNS_ANSWER_TTL_S >> 8) & 0xFF;
    *p++ = DNS_ANSWER_TTL_S & 0xFF;

    // Länge der Daten (RDLENGTH)
    *p++ = 0x00; *p++ = DNS_RDLENGTH_IPV4;
    
    // Füge die IP-Adresse des APs als Antwort ein
    memcpy(p, &ap_ip->addr, DNS_RDLENGTH_IPV4);
}

/**
 * @brief Der FreeRTOS-Task, der den DNS-Server ausführt.
 */
static void dns_server_task(void *pvParameters) {
    uint8_t request_buffer[DNS_MAX_LEN];
    uint8_t response_buffer[DNS_MAX_LEN];
    struct sockaddr client;
    socklen_t client_len = sizeof(client);

    // Hole die IP-Informationen des Access-Point-Interfaces
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);

    // Erstelle und konfiguriere die Server-Adresse für den Socket.
    // Wir verwenden die spezifische `sockaddr`-Struktur für IPv4.
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Lausche auf allen IPs des Geräts
    server_addr.sin_port = htons(DNS_PORT);   // Lausche auf Port 53

    // Erstelle einen UDP-Socket
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    // Binde den Socket an die konfigurierte Adresse und den Port.
    // Die bind()-Funktion erwartet einen Pointer auf die generische `sockaddr`-Struktur,
    // daher casten wir den Pointer unserer `sockaddr`-Struktur.
    if (bind(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket");
        close(sock_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS Server started on port %d", DNS_PORT);
    dns_server_running = true;

    // Hauptschleife zum Empfangen und Beantworten von DNS-Anfragen
    while (dns_server_running) {
        int len = recvfrom(sock_fd, request_buffer, sizeof(request_buffer), 0, (struct sockaddr *)&client, &client_len);
        if (len > 0) {
            // Ignoriere Anfragen, die bereits Antworten sind
            if ((request_buffer[2] & DNS_FLAG_QR) == 0) {
                ESP_LOGI(TAG, "DNS request...");
                create_dns_response(request_buffer, response_buffer, &ip_info.ip);
                sendto(sock_fd, response_buffer, len + DNS_ANSWER_LEN, 0, (struct sockaddr *)&client, client_len);
            }
        }
    }

    // Aufräumen, wenn die Schleife beendet wird
    close(sock_fd);
    sock_fd = -1;
    vTaskDelete(NULL);
}

/**
 * @brief Startet den DNS-Server-Task.
 */
void start_dns_server() {
    if (!dns_server_running) {
        xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
    }
}

/**
 * @brief Stoppt den DNS-Server-Task.
 */
void stop_dns_server() {
    if (dns_server_running) {
        dns_server_running = false;
        // Schließe den Socket, um den blockierenden recvfrom()-Aufruf zu beenden
        if (sock_fd != -1) {
            shutdown(sock_fd, SHUT_RDWR);
        }
    }
}