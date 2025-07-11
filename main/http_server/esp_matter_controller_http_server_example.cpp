/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <esp_matter_controller_http_server.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>

static const char *TAG = "http_server_example";

namespace esp_matter {
namespace controller {
namespace http_server {

/**
 * @brief Initialize HTTP server with Matter controller
 * This function should be called after Matter controller initialization
 * and WiFi connection is established
 */
esp_err_t initialize_http_server_with_controller() {
    ESP_LOGI(TAG, "Initializing HTTP server for Matter controller");
    
    // Check if WiFi is connected
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi not connected, HTTP server may not be accessible");
    }
    
    // Get IP address
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "Device IP: " IPSTR, IP2STR(&ip_info.ip));
    }
    
    // Configure HTTP server
    http_server_config_t config = HTTP_SERVER_DEFAULT_CONFIG();
    config.port = 8080;
    config.cors_enable = true;
    config.max_uri_handlers = 20;
    config.max_open_sockets = 7;
    
    // Start HTTP server
    esp_err_t ret = start_http_server(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "HTTP server started successfully on port %d", config.port);
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "Access the API at: http://" IPSTR ":%d/api/help", IP2STR(&ip_info.ip), config.port);
    }
    
    return ESP_OK;
}

/**
 * @brief Stop HTTP server
 */
esp_err_t deinitialize_http_server() {
    ESP_LOGI(TAG, "Stopping HTTP server");
    return stop_http_server();
}

/**
 * @brief Example WiFi event handler that starts HTTP server when WiFi is connected
 */
void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi started, attempting to connect");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // Start HTTP server when IP is obtained
        esp_err_t ret = initialize_http_server_with_controller();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize HTTP server: %s", esp_err_to_name(ret));
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, stopping HTTP server");
        deinitialize_http_server();
    }
}

/**
 * @brief Example function to integrate HTTP server with Matter controller application
 * Call this function from your main application after Matter controller initialization
 */
esp_err_t setup_matter_controller_with_http_server() {
    ESP_LOGI(TAG, "Setting up Matter controller with HTTP server");
    
    // Register WiFi event handler
    // ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    
    ESP_LOGI(TAG, "HTTP server will start automatically when WiFi is connected");
    return ESP_OK;
}

} // namespace http_server
} // namespace controller
} // namespace esp_matter

/* 
 * Integration example for existing controller applications:
 * 
 * 1. Add to your CMakeLists.txt:
 *    set(COMPONENT_REQUIRES ... esp_http_server json)
 * 
 * 2. Add to your main application:
 *    #include <esp_matter_controller_http_server.h>
 * 
 * 3. Call after Matter controller initialization:
 *    esp_matter::controller::http_server::setup_matter_controller_with_http_server();
 * 
 * 4. Or manually start the server:
 *    esp_matter::controller::http_server::http_server_config_t config = 
 *        ESP_MATTER_CONTROLLER_HTTP_SERVER_DEFAULT_CONFIG();
 *    esp_matter::controller::http_server::start_http_server(&config);
 * 
 * 5. Access the API:
 *    GET  http://device-ip:8080/api/help
 *    POST http://device-ip:8080/api/pairing
 *    POST http://device-ip:8080/api/invoke-command
 *    ... and more
 */ 