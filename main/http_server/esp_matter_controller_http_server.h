#pragma once

#include <esp_err.h>
#include <esp_http_server.h>
#include <cJSON.h>

namespace esp_matter {
namespace controller {
namespace http_server {

/**
 * @brief HTTP Server configuration structure
 */
typedef struct {
    uint16_t port;          // HTTP server port
    size_t max_uri_handlers; // Maximum number of URI handlers
    size_t max_resp_headers; // Maximum number of additional response headers
    size_t max_open_sockets; // Maximum number of open sockets
    bool cors_enable;        // Enable CORS headers
} http_server_config_t;

/**
 * @brief Default HTTP server configuration
 */
#define HTTP_SERVER_DEFAULT_CONFIG() {       \
    .port = 8080,                            \
    .max_uri_handlers = 50,                  \
    .max_resp_headers = 8,                   \
    .max_open_sockets = 7,                   \
    .cors_enable = true                      \
}

/**
 * @brief Initialize and start the HTTP server
 * @param config HTTP server configuration
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t start_http_server(const http_server_config_t *config);

/**
 * @brief Stop the HTTP server
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t stop_http_server(void);

/**
 * @brief Get the HTTP server handle
 * @return HTTP server handle or NULL if not started
 */
httpd_handle_t get_http_server_handle(void);

/**
 * @brief Check if HTTP server is running
 * @return true if running, false otherwise
 */
bool is_http_server_running(void);

// REST API Handler functions
esp_err_t pairing_handler(httpd_req_t *req);
esp_err_t group_settings_handler(httpd_req_t *req);
esp_err_t udc_handler(httpd_req_t *req);
esp_err_t open_commissioning_window_handler(httpd_req_t *req);
esp_err_t invoke_command_handler(httpd_req_t *req);
esp_err_t read_attribute_handler(httpd_req_t *req);
esp_err_t write_attribute_handler(httpd_req_t *req);
esp_err_t read_event_handler(httpd_req_t *req);
esp_err_t subscribe_attribute_handler(httpd_req_t *req);
esp_err_t subscribe_event_handler(httpd_req_t *req);
esp_err_t shutdown_subscription_handler(httpd_req_t *req);
esp_err_t shutdown_all_subscriptions_handler(httpd_req_t *req);
esp_err_t ble_scan_handler(httpd_req_t *req);
esp_err_t help_handler(httpd_req_t *req);

// Utility functions
esp_err_t send_json_response(httpd_req_t *req, cJSON *json, int status_code = 200);
esp_err_t send_error_response(httpd_req_t *req, int status_code, const char *error_message);
esp_err_t parse_json_request(httpd_req_t *req, cJSON **json);
esp_err_t add_cors_headers(httpd_req_t *req);

esp_err_t setup_matter_controller_with_http_server();
esp_err_t initialize_http_server_with_controller();

} // namespace http_server
} // namespace controller
} // namespace esp_matter