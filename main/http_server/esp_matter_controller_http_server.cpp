
/*
 *
 *    Copyright (c) 2022 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <esp_check.h>
#include <esp_log.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_controller_commissioning_window_opener.h>
#include <esp_matter_controller_console.h>
#include <esp_matter_controller_group_settings.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_matter_controller_read_command.h>
#include <esp_matter_controller_subscribe_command.h>
#include <esp_matter_controller_utils.h>
#include <esp_matter_controller_write_command.h>
#include <esp_matter_controller_http_server.h>
#include <esp_matter_core.h>
#include <algorithm>
#if CONFIG_ENABLE_ESP32_CONTROLLER_BLE_SCAN
#include <esp_matter_controller_ble_scan_command.h>
#endif
#include <esp_netif.h>
#include <inttypes.h>
#include <lib/core/CHIPCore.h>
#include <lib/shell/Commands.h>
#include <lib/shell/Engine.h>
#include <lib/shell/commands/Help.h>
#include <lib/shell/streamer.h>
#include <lib/support/CHIPArgParser.hpp>
#include <lib/support/CHIPMem.h>
#include <lib/support/CodeUtils.h>
#include <platform/CHIPDeviceLayer.h>
#include <protocols/secure_channel/RendezvousParameters.h>
#include <protocols/user_directed_commissioning/UserDirectedCommissioning.h>
#include <string.h>

using chip::NodeId;
using chip::Inet::IPAddress;
using chip::Platform::ScopedMemoryBufferWithSize;
using chip::Transport::PeerAddress;

namespace esp_matter {
namespace controller {
namespace http_server {

static const char *TAG = "controller_httpserver";
static httpd_handle_t s_server = NULL;
static bool s_cors_enabled = false;

// Simple lock helper - returns true if lock acquired successfully
static bool acquire_matter_lock() {
    esp_matter::lock::status_t status = esp_matter::lock::chip_stack_lock(pdMS_TO_TICKS(2000)); // Reduced to 2 seconds
    return (status == esp_matter::lock::SUCCESS);
}

static void release_matter_lock() {
    esp_matter::lock::chip_stack_unlock();
}

// Safe logging function that doesn't use locks during Matter operations
static void safe_log(const char* level, const char* message) {
    // Use printf instead of ESP_LOG to avoid potential lock conflicts
    printf("[%s] %s: %s\n", level, TAG, message);
}

// Enhanced error handling with stack trace protection
static esp_err_t safe_send_error_response(httpd_req_t *req, int status_code, const char *error_message) {
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        // Fallback to simple HTTP response if JSON creation fails
        const char *simple_error = "Internal server error";
        httpd_resp_set_status(req, status_code == 500 ? HTTPD_500 : HTTPD_400);
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, simple_error, strlen(simple_error));
        return ESP_OK;
    }
    
    cJSON_AddStringToObject(json, "error", error_message);
    cJSON_AddNumberToObject(json, "status", status_code);
    
    esp_err_t ret = send_json_response(req, json, status_code);
    cJSON_Delete(json);
    return ret;
}

// Utility functions
static size_t get_array_size(const char *str)
{
    if (!str) {
        return 0;
    }
    size_t ret = 1;
    for (size_t i = 0; i < strlen(str); ++i) {
        if (str[i] == ',') {
            ret++;
        }
    }
    return ret;
}

static esp_err_t string_to_uint32_array(const char *str, ScopedMemoryBufferWithSize<uint32_t> &uint32_array)
{
    size_t array_len = get_array_size(str);
    if (array_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_array.Calloc(array_len);
    if (!uint32_array.Get()) {
        return ESP_ERR_NO_MEM;
    }
    char number[11]; // max(strlen("0xFFFFFFFF"), strlen("4294967295")) + 1
    const char *next_number_start = str;
    char *next_number_end = NULL;
    size_t next_number_len = 0;
    for (size_t i = 0; i < array_len; ++i) {
        next_number_end = strchr(next_number_start, ',');
        if (next_number_end > next_number_start) {
            next_number_len = std::min((size_t)(next_number_end - next_number_start), sizeof(number) - 1);
        } else if (i == array_len - 1) {
            next_number_len = strnlen(next_number_start, sizeof(number) - 1);
        } else {
            return ESP_ERR_INVALID_ARG;
        }
        strncpy(number, next_number_start, next_number_len);
        number[next_number_len] = 0;
        uint32_array[i] = string_to_uint32(number);
        if (next_number_end > next_number_start) {
            next_number_start = next_number_end + 1;
        }
    }
    return ESP_OK;
}

static esp_err_t string_to_uint16_array(const char *str, ScopedMemoryBufferWithSize<uint16_t> &uint16_array)
{
    size_t array_len = get_array_size(str);
    if (array_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_array.Calloc(array_len);
    if (!uint16_array.Get()) {
        return ESP_ERR_NO_MEM;
    }
    char number[7]; // max(strlen(0xFFFF), strlen(65535)) + 1
    const char *next_number_start = str;
    char *next_number_end = NULL;
    size_t next_number_len = 0;
    for (size_t i = 0; i < array_len; ++i) {
        next_number_end = strchr(next_number_start, ',');
        if (next_number_end > next_number_start) {
            next_number_len = std::min((size_t)(next_number_end - next_number_start), sizeof(number) - 1);
        } else if (i == array_len - 1) {
            next_number_len = strnlen(next_number_start, sizeof(number) - 1);
        } else {
            return ESP_ERR_INVALID_ARG;
        }
        strncpy(number, next_number_start, next_number_len);
        number[next_number_len] = 0;
        uint16_array[i] = string_to_uint16(number);
        if (next_number_end > next_number_start) {
            next_number_start = next_number_end + 1;
        }
    }
    return ESP_OK;
}

#if defined(CONFIG_ENABLE_ESP32_CONTROLLER_BLE_SCAN) && defined(CONFIG_ESP_MATTER_COMMISSIONER_ENABLE)
static int char_to_int(char ch)
{
    if ('A' <= ch && ch <= 'F') {
        return 10 + ch - 'A';
    } else if ('a' <= ch && ch <= 'f') {
        return 10 + ch - 'a';
    } else if ('0' <= ch && ch <= '9') {
        return ch - '0';
    }
    return -1;
}

static bool convert_hex_str_to_bytes(const char *hex_str, uint8_t *bytes, uint8_t &bytes_len)
{
    if (!hex_str || !bytes) {
        return false;
    }
    size_t hex_str_len = strlen(hex_str);
    if (hex_str_len == 0 || hex_str_len % 2 != 0 || hex_str_len / 2 > bytes_len) {
        return false;
    }
    uint8_t output_len = hex_str_len / 2;
    for (size_t i = 0; i < output_len; ++i) {
        int byte_h = char_to_int(hex_str[2 * i]);
        int byte_l = char_to_int(hex_str[2 * i + 1]);
        if (byte_h < 0 || byte_l < 0) {
            return false;
        }
        bytes[i] = (byte_h << 4) + byte_l;
    }
    bytes_len = output_len;
    return true;
}
#endif // defined(CONFIG_ENABLE_ESP32_CONTROLLER_BLE_SCAN) && defined(CONFIG_ESP_MATTER_COMMISSIONER_ENABLE)

esp_err_t add_cors_headers(httpd_req_t *req) {
    if (!s_cors_enabled) {
        return ESP_OK;
    }
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    
    return ESP_OK;
}

esp_err_t send_json_response(httpd_req_t *req, cJSON *json, int status_code) {
    char *json_string = cJSON_Print(json);
    if (!json_string) {
        return ESP_ERR_NO_MEM;
    }
    
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status_code == 200 ? HTTPD_200 : 
                                status_code == 400 ? HTTPD_400 :
                                status_code == 500 ? HTTPD_500 : HTTPD_400);
    
    esp_err_t ret = httpd_resp_send(req, json_string, strlen(json_string));
    free(json_string);
    return ret;
}

esp_err_t send_error_response(httpd_req_t *req, int status_code, const char *error_message) {
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(json, "error", error_message);
    cJSON_AddNumberToObject(json, "status", status_code);
    
    esp_err_t ret = send_json_response(req, json, status_code);
    cJSON_Delete(json);
    return ret;
}

esp_err_t parse_json_request(httpd_req_t *req, cJSON **json) {
    if (req->content_len == 0) {
        *json = cJSON_CreateObject();
        return ESP_OK;
    }
    
    char *buf = (char *)malloc(req->content_len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        free(buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    buf[received] = '\0';
    *json = cJSON_Parse(buf);
    free(buf);
    
    if (!*json) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}

// OPTIONS handler for CORS preflight requests
esp_err_t options_handler(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// API: GET /api/help - Get available endpoints
esp_err_t help_handler(httpd_req_t *req) {
    cJSON *json = cJSON_CreateObject();
    cJSON *endpoints = cJSON_CreateArray();
    
    // Add all available endpoints
    cJSON *endpoint;
    
    endpoint = cJSON_CreateObject();
    cJSON_AddStringToObject(endpoint, "path", "/api/pairing");
    cJSON_AddStringToObject(endpoint, "method", "POST");
    cJSON_AddStringToObject(endpoint, "description", "Pair a device to the controller");
    cJSON_AddItemToArray(endpoints, endpoint);
    
    endpoint = cJSON_CreateObject();
    cJSON_AddStringToObject(endpoint, "path", "/api/group-settings");
    cJSON_AddStringToObject(endpoint, "method", "POST");
    cJSON_AddStringToObject(endpoint, "description", "Manage controller groups and keysets");
    cJSON_AddItemToArray(endpoints, endpoint);
    
    endpoint = cJSON_CreateObject();
    cJSON_AddStringToObject(endpoint, "path", "/api/udc");
    cJSON_AddStringToObject(endpoint, "method", "POST");
    cJSON_AddStringToObject(endpoint, "description", "UDC (User Directed Commissioning) commands");
    cJSON_AddItemToArray(endpoints, endpoint);
    
    endpoint = cJSON_CreateObject();
    cJSON_AddStringToObject(endpoint, "path", "/api/open-commissioning-window");
    cJSON_AddStringToObject(endpoint, "method", "POST");
    cJSON_AddStringToObject(endpoint, "description", "Open commissioning window on a device");
    cJSON_AddItemToArray(endpoints, endpoint);
    
    endpoint = cJSON_CreateObject();
    cJSON_AddStringToObject(endpoint, "path", "/api/invoke-command");
    cJSON_AddStringToObject(endpoint, "method", "POST");
    cJSON_AddStringToObject(endpoint, "description", "Invoke cluster command on a device");
    cJSON_AddItemToArray(endpoints, endpoint);
    
    endpoint = cJSON_CreateObject();
    cJSON_AddStringToObject(endpoint, "path", "/api/read-attribute");
    cJSON_AddStringToObject(endpoint, "method", "POST");
    cJSON_AddStringToObject(endpoint, "description", "Read device attributes");
    cJSON_AddItemToArray(endpoints, endpoint);
    
    endpoint = cJSON_CreateObject();
    cJSON_AddStringToObject(endpoint, "path", "/api/write-attribute");
    cJSON_AddStringToObject(endpoint, "method", "POST");
    cJSON_AddStringToObject(endpoint, "description", "Write device attributes");
    cJSON_AddItemToArray(endpoints, endpoint);
    
    endpoint = cJSON_CreateObject();
    cJSON_AddStringToObject(endpoint, "path", "/api/read-event");
    cJSON_AddStringToObject(endpoint, "method", "POST");
    cJSON_AddStringToObject(endpoint, "description", "Read device events");
    cJSON_AddItemToArray(endpoints, endpoint);
    
    endpoint = cJSON_CreateObject();
    cJSON_AddStringToObject(endpoint, "path", "/api/subscribe-attribute");
    cJSON_AddStringToObject(endpoint, "method", "POST");
    cJSON_AddStringToObject(endpoint, "description", "Subscribe to device attributes");
    cJSON_AddItemToArray(endpoints, endpoint);
    
    endpoint = cJSON_CreateObject();
    cJSON_AddStringToObject(endpoint, "path", "/api/subscribe-event");
    cJSON_AddStringToObject(endpoint, "method", "POST");
    cJSON_AddStringToObject(endpoint, "description", "Subscribe to device events");
    cJSON_AddItemToArray(endpoints, endpoint);
    
    endpoint = cJSON_CreateObject();
    cJSON_AddStringToObject(endpoint, "path", "/api/shutdown-subscription");
    cJSON_AddStringToObject(endpoint, "method", "POST");
    cJSON_AddStringToObject(endpoint, "description", "Shutdown specific subscription");
    cJSON_AddItemToArray(endpoints, endpoint);
    
    endpoint = cJSON_CreateObject();
    cJSON_AddStringToObject(endpoint, "path", "/api/shutdown-all-subscriptions");
    cJSON_AddStringToObject(endpoint, "method", "POST");
    cJSON_AddStringToObject(endpoint, "description", "Shutdown all subscriptions");
    cJSON_AddItemToArray(endpoints, endpoint);
    
#if CONFIG_ENABLE_ESP32_CONTROLLER_BLE_SCAN
    endpoint = cJSON_CreateObject();
    cJSON_AddStringToObject(endpoint, "path", "/api/ble-scan");
    cJSON_AddStringToObject(endpoint, "method", "POST");
    cJSON_AddStringToObject(endpoint, "description", "Scan for BLE devices");
    cJSON_AddItemToArray(endpoints, endpoint);
#endif
    
    cJSON_AddItemToObject(json, "endpoints", endpoints);
    cJSON_AddStringToObject(json, "version", "1.0.0");
    cJSON_AddStringToObject(json, "description", "ESP Matter Controller REST API");
    
    esp_err_t ret = send_json_response(req, json, 200);
    cJSON_Delete(json);
    return ret;
}

// API: POST /api/pairing - Pair device
esp_err_t pairing_handler(httpd_req_t *req) {
    cJSON *json = NULL;
    esp_err_t ret = parse_json_request(req, &json);
    if (ret != ESP_OK) {
        return send_error_response(req, 400, "Invalid JSON");
    }
    
    cJSON *method = cJSON_GetObjectItem(json, "method");
    if (!method || !cJSON_IsString(method)) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Missing or invalid 'method' field");
    }
    
    cJSON *response = cJSON_CreateObject();
    esp_err_t result = ESP_FAIL;
    
    if (strcmp(method->valuestring, "onnetwork") == 0) {
        cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
        cJSON *pincode = cJSON_GetObjectItem(json, "pincode");
        
        if (!node_id || !pincode || !cJSON_IsString(node_id) || !cJSON_IsString(pincode) ||
            !node_id->valuestring || !pincode->valuestring) {
            cJSON_Delete(json);
            cJSON_Delete(response);
            return send_error_response(req, 400, "Missing or invalid node_id or pincode for onnetwork pairing");
        }
        
        uint64_t nodeId = string_to_uint64(node_id->valuestring);
        uint32_t pin = string_to_uint32(pincode->valuestring);
        
        // Lock Matter stack with timeout
        if (!acquire_matter_lock()) {
            cJSON_Delete(json);
            cJSON_Delete(response);
            return send_error_response(req, 500, "Matter stack busy - timeout acquiring lock");
        }
        
        result = controller::pairing_on_network(nodeId, pin);
        release_matter_lock();
        
    } else if (strcmp(method->valuestring, "ble-wifi") == 0) {
#if CONFIG_ENABLE_ESP32_CONTROLLER_BLE_SCAN
        cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
        cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
        cJSON *password = cJSON_GetObjectItem(json, "password");
        cJSON *pincode = cJSON_GetObjectItem(json, "pincode");
        cJSON *discriminator = cJSON_GetObjectItem(json, "discriminator");
        
        if (!node_id || !ssid || !password || !pincode || !discriminator ||
            !cJSON_IsString(node_id) || !cJSON_IsString(ssid) || !cJSON_IsString(password) ||
            !cJSON_IsString(pincode) || !cJSON_IsString(discriminator) ||
            !node_id->valuestring || !ssid->valuestring || !password->valuestring ||
            !pincode->valuestring || !discriminator->valuestring) {
            cJSON_Delete(json);
            cJSON_Delete(response);
            return send_error_response(req, 400, "Missing or invalid parameters for ble-wifi pairing");
        }
        
        uint64_t nodeId = string_to_uint64(node_id->valuestring);
        uint32_t pin = string_to_uint32(pincode->valuestring);
        uint16_t disc = string_to_uint16(discriminator->valuestring);
        
        if (!acquire_matter_lock()) {
            cJSON_Delete(json);
            cJSON_Delete(response);
            return send_error_response(req, 500, "Matter stack busy - timeout acquiring lock");
        }
        
        result = controller::pairing_ble_wifi(nodeId, pin, disc, ssid->valuestring, password->valuestring);
        release_matter_lock();
    } else if (strcmp(method->valuestring, "ble-thread") == 0) {
        cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
        cJSON *dataset = cJSON_GetObjectItem(json, "dataset");
        cJSON *pincode = cJSON_GetObjectItem(json, "pincode");
        cJSON *discriminator = cJSON_GetObjectItem(json, "discriminator");
        
        if (!node_id || !dataset || !pincode || !discriminator ||
            !cJSON_IsString(node_id) || !cJSON_IsString(dataset) || 
            !cJSON_IsString(pincode) || !cJSON_IsString(discriminator)) {
            cJSON_Delete(json);
            cJSON_Delete(response);
            return send_error_response(req, 400, "Missing or invalid parameters for ble-thread pairing");
        }
        
        // Check if dataset string is not null
        if (!dataset->valuestring || strlen(dataset->valuestring) == 0) {
            cJSON_Delete(json);
            cJSON_Delete(response);
            return send_error_response(req, 400, "Dataset cannot be empty");
        }
        
        uint8_t dataset_tlvs_buf[254];
        uint8_t dataset_tlvs_len = sizeof(dataset_tlvs_buf);
        if (!convert_hex_str_to_bytes(dataset->valuestring, dataset_tlvs_buf, dataset_tlvs_len)) {
            cJSON_Delete(json);
            cJSON_Delete(response);
            return send_error_response(req, 400, "Invalid dataset format - must be hex string");
        }
        
        uint64_t nodeId = string_to_uint64(node_id->valuestring);
        uint32_t pin = string_to_uint32(pincode->valuestring);
        uint16_t disc = string_to_uint16(discriminator->valuestring);
        
        // Lock the Matter stack before calling pairing function
        esp_matter::lock::status_t lock_status = esp_matter::lock::chip_stack_lock(portMAX_DELAY);
        if (lock_status != esp_matter::lock::SUCCESS) {
            ESP_LOGE(TAG, "Failed to acquire Matter stack lock");
            cJSON_Delete(json);
            cJSON_Delete(response);
            return send_error_response(req, 500, "Internal server error - failed to acquire lock");
        }
        
        result = controller::pairing_ble_thread(nodeId, pin, disc, dataset_tlvs_buf, dataset_tlvs_len);
        esp_matter::lock::chip_stack_unlock();
#else
        cJSON_Delete(json);
        cJSON_Delete(response);
        return send_error_response(req, 400, "BLE pairing not supported - CONFIG_ENABLE_ESP32_CONTROLLER_BLE_SCAN disabled");
#endif
    } else if (strcmp(method->valuestring, "code") == 0) {
        cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
        cJSON *payload = cJSON_GetObjectItem(json, "payload");
        
        if (!node_id || !payload || !cJSON_IsString(node_id) || !cJSON_IsString(payload) ||
            !node_id->valuestring || !payload->valuestring) {
            cJSON_Delete(json);
            cJSON_Delete(response);
            return send_error_response(req, 400, "Missing or invalid node_id or payload for code pairing");
        }
        
        uint64_t nodeId = string_to_uint64(node_id->valuestring);
        
        if (!acquire_matter_lock()) {
            cJSON_Delete(json);
            cJSON_Delete(response);
            return send_error_response(req, 500, "Matter stack busy - timeout acquiring lock");
        }
        
        result = controller::pairing_code(nodeId, payload->valuestring);
        release_matter_lock();
        
    } else {
        cJSON_Delete(json);
        cJSON_Delete(response);
        return send_error_response(req, 400, "Unsupported pairing method");
    }
    
    if (result == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Pairing command sent successfully");
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Pairing command failed");
    }
    
    ret = send_json_response(req, response, result == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ret;
}

// API: POST /api/open-commissioning-window - Open commissioning window
esp_err_t open_commissioning_window_handler(httpd_req_t *req) {
    cJSON *json = NULL;
    esp_err_t ret = parse_json_request(req, &json);
    if (ret != ESP_OK) {
        return send_error_response(req, 400, "Invalid JSON");
    }
    
    cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
    cJSON *option = cJSON_GetObjectItem(json, "option");
    cJSON *window_timeout = cJSON_GetObjectItem(json, "window_timeout");
    cJSON *iteration = cJSON_GetObjectItem(json, "iteration");
    cJSON *discriminator = cJSON_GetObjectItem(json, "discriminator");
    
    if (!node_id || !option || !window_timeout || !iteration || !discriminator ||
        !cJSON_IsString(node_id) || !cJSON_IsString(option) || !cJSON_IsString(window_timeout) ||
        !cJSON_IsString(iteration) || !cJSON_IsString(discriminator) ||
        !node_id->valuestring || !option->valuestring || !window_timeout->valuestring ||
        !iteration->valuestring || !discriminator->valuestring) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Missing or invalid required parameters");
    }
    
    uint64_t nodeId = string_to_uint64(node_id->valuestring);
    uint8_t opt = string_to_uint8(option->valuestring);
    bool is_enhanced = opt == 1;
    uint16_t timeout = string_to_uint16(window_timeout->valuestring);
    uint32_t iter = string_to_uint32(iteration->valuestring);
    uint16_t disc = string_to_uint16(discriminator->valuestring);
    
    // Lock the Matter stack before calling commissioning window command
    esp_matter::lock::status_t lock_status = esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (lock_status != esp_matter::lock::SUCCESS) {
        ESP_LOGE(TAG, "Failed to acquire Matter stack lock");
        cJSON_Delete(json);
        return send_error_response(req, 500, "Internal server error - failed to acquire lock");
    }
    
    esp_err_t result = controller::commissioning_window_opener::get_instance().send_open_commissioning_window_command(
        nodeId, is_enhanced, timeout, iter, disc, 10000);
    esp_matter::lock::chip_stack_unlock();
    
    cJSON *response = cJSON_CreateObject();
    if (result == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Commissioning window opened successfully");
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Failed to open commissioning window");
    }
    
    ret = send_json_response(req, response, result == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ret;
}

// API: POST /api/invoke-command - Invoke cluster command
esp_err_t invoke_command_handler(httpd_req_t *req) {
    cJSON *json = NULL;
    esp_err_t ret = parse_json_request(req, &json);
    if (ret != ESP_OK) {
        return send_error_response(req, 400, "Invalid JSON");
    }
    
    cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
    cJSON *endpoint_id = cJSON_GetObjectItem(json, "endpoint_id");
    cJSON *cluster_id = cJSON_GetObjectItem(json, "cluster_id");
    cJSON *command_id = cJSON_GetObjectItem(json, "command_id");
    cJSON *command_data = cJSON_GetObjectItem(json, "command_data");
    cJSON *timed_invoke_timeout = cJSON_GetObjectItem(json, "timed_invoke_timeout_ms");
    
    if (!node_id || !endpoint_id || !cluster_id || !command_id ||
        !cJSON_IsString(node_id) || !cJSON_IsString(endpoint_id) ||
        !cJSON_IsString(cluster_id) || !cJSON_IsString(command_id) ||
        !node_id->valuestring || !endpoint_id->valuestring ||
        !cluster_id->valuestring || !command_id->valuestring) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Missing or invalid required parameters");
    }
    
    uint64_t nodeId = string_to_uint64(node_id->valuestring);
    uint16_t epId = string_to_uint16(endpoint_id->valuestring);
    uint32_t clusterId = string_to_uint32(cluster_id->valuestring);
    uint32_t cmdId = string_to_uint32(command_id->valuestring);
    
    char *cmd_data_str = NULL;
    if (command_data && cJSON_IsString(command_data)) {
        cmd_data_str = command_data->valuestring;
    }
    
    // Lock Matter stack with timeout
    if (!acquire_matter_lock()) {
        cJSON_Delete(json);
        return send_error_response(req, 500, "Matter stack busy - timeout acquiring lock");
    }
    
    esp_err_t result;
    if (timed_invoke_timeout && cJSON_IsNumber(timed_invoke_timeout) && timed_invoke_timeout->valueint > 0) {
        result = controller::send_invoke_cluster_command(nodeId, epId, clusterId, cmdId, cmd_data_str,
                                                        chip::MakeOptional((uint16_t)timed_invoke_timeout->valueint));
    } else {
        result = controller::send_invoke_cluster_command(nodeId, epId, clusterId, cmdId, cmd_data_str);
    }
    release_matter_lock();
    
    cJSON *response = cJSON_CreateObject();
    if (result == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Command invoked successfully");
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Failed to invoke command");
    }
    
    ret = send_json_response(req, response, result == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ret;
}

// API: POST /api/read-attribute - Read attributes
esp_err_t read_attribute_handler(httpd_req_t *req) {
    cJSON *json = NULL;
    esp_err_t ret = parse_json_request(req, &json);
    if (ret != ESP_OK) {
        return safe_send_error_response(req, 400, "Invalid JSON");
    }
    
    cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
    cJSON *endpoint_ids = cJSON_GetObjectItem(json, "endpoint_ids");
    cJSON *cluster_ids = cJSON_GetObjectItem(json, "cluster_ids");
    cJSON *attribute_ids = cJSON_GetObjectItem(json, "attribute_ids");
    
    if (!node_id || !endpoint_ids || !cluster_ids || !attribute_ids ||
        !cJSON_IsString(node_id) || !cJSON_IsString(endpoint_ids) ||
        !cJSON_IsString(cluster_ids) || !cJSON_IsString(attribute_ids) ||
        !node_id->valuestring || !endpoint_ids->valuestring ||
        !cluster_ids->valuestring || !attribute_ids->valuestring) {
        cJSON_Delete(json);
        return safe_send_error_response(req, 400, "Missing or invalid required parameters");
    }
    
    uint64_t nodeId = string_to_uint64(node_id->valuestring);
    
    ScopedMemoryBufferWithSize<uint16_t> ep_ids;
    ScopedMemoryBufferWithSize<uint32_t> cl_ids;
    ScopedMemoryBufferWithSize<uint32_t> attr_ids;
    
    esp_err_t result = ESP_FAIL;
    
    // Pre-validate and allocate all arrays before acquiring lock
    if (string_to_uint16_array(endpoint_ids->valuestring, ep_ids) != ESP_OK) {
        cJSON_Delete(json);
        return safe_send_error_response(req, 400, "Invalid endpoint_ids format");
    }
    
    if (string_to_uint32_array(cluster_ids->valuestring, cl_ids) != ESP_OK) {
        cJSON_Delete(json);
        return safe_send_error_response(req, 400, "Invalid cluster_ids format");
    }
    
    if (string_to_uint32_array(attribute_ids->valuestring, attr_ids) != ESP_OK) {
        cJSON_Delete(json);
        return safe_send_error_response(req, 400, "Invalid attribute_ids format");
    }
    
    // Minimize time spent in lock by preparing everything beforehand
    bool lock_acquired = false;
    
    // Try to acquire lock with shorter timeout
    if (!acquire_matter_lock()) {
        cJSON_Delete(json);
        return safe_send_error_response(req, 503, "Matter stack busy - please retry");
    }
    
    lock_acquired = true;
    
    // Execute command as quickly as possible while locked
    result = controller::send_read_attr_command(nodeId, ep_ids, cl_ids, attr_ids);
    
    // Release lock immediately after command
    release_matter_lock();
    lock_acquired = false;
    
    // Create response after releasing lock
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        cJSON_Delete(json);
        return safe_send_error_response(req, 500, "Failed to create response");
    }
    
    if (result == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Read attribute command sent successfully");
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Failed to send read attribute command");
    }
    
    ret = send_json_response(req, response, result == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ret;
}

// API: POST /api/write-attribute - Write attributes
esp_err_t write_attribute_handler(httpd_req_t *req) {
    cJSON *json = NULL;
    esp_err_t ret = parse_json_request(req, &json);
    if (ret != ESP_OK) {
        return send_error_response(req, 400, "Invalid JSON");
    }
    
    cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
    cJSON *endpoint_ids = cJSON_GetObjectItem(json, "endpoint_ids");
    cJSON *cluster_ids = cJSON_GetObjectItem(json, "cluster_ids");
    cJSON *attribute_ids = cJSON_GetObjectItem(json, "attribute_ids");
    cJSON *attribute_value = cJSON_GetObjectItem(json, "attribute_value");
    cJSON *timed_write_timeout = cJSON_GetObjectItem(json, "timed_write_timeout_ms");
    
    if (!node_id || !endpoint_ids || !cluster_ids || !attribute_ids || !attribute_value ||
        !cJSON_IsString(node_id) || !cJSON_IsString(endpoint_ids) ||
        !cJSON_IsString(cluster_ids) || !cJSON_IsString(attribute_ids) ||
        !cJSON_IsString(attribute_value) ||
        !node_id->valuestring || !endpoint_ids->valuestring ||
        !cluster_ids->valuestring || !attribute_ids->valuestring ||
        !attribute_value->valuestring) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Missing or invalid required parameters");
    }
    
    uint64_t nodeId = string_to_uint64(node_id->valuestring);
    
    ScopedMemoryBufferWithSize<uint16_t> ep_ids;
    ScopedMemoryBufferWithSize<uint32_t> cl_ids;
    ScopedMemoryBufferWithSize<uint32_t> attr_ids;
    
    esp_err_t result = ESP_FAIL;
    
    // Pre-validate and allocate all arrays before acquiring lock
    if (string_to_uint16_array(endpoint_ids->valuestring, ep_ids) != ESP_OK) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Invalid endpoint_ids format");
    }
    
    if (string_to_uint32_array(cluster_ids->valuestring, cl_ids) != ESP_OK) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Invalid cluster_ids format");
    }
    
    if (string_to_uint32_array(attribute_ids->valuestring, attr_ids) != ESP_OK) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Invalid attribute_ids format");
    }
    
    // Lock Matter stack with timeout
    if (!acquire_matter_lock()) {
        cJSON_Delete(json);
        return send_error_response(req, 500, "Matter stack busy - timeout acquiring lock");
    }
    
    if (timed_write_timeout && cJSON_IsNumber(timed_write_timeout) && timed_write_timeout->valueint > 0) {
        result = controller::send_write_attr_command(nodeId, ep_ids, cl_ids, attr_ids, attribute_value->valuestring,
                                                    chip::MakeOptional((uint16_t)timed_write_timeout->valueint));
    } else {
        result = controller::send_write_attr_command(nodeId, ep_ids, cl_ids, attr_ids, attribute_value->valuestring);
    }
    release_matter_lock();
    
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        cJSON_Delete(json);
        return send_error_response(req, 500, "Failed to create response");
    }
    
    if (result == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Write attribute command sent successfully");
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Failed to send write attribute command");
    }
    
    ret = send_json_response(req, response, result == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ret;
}

// API: POST /api/read-event - Read events
esp_err_t read_event_handler(httpd_req_t *req) {
    cJSON *json = NULL;
    esp_err_t ret = parse_json_request(req, &json);
    if (ret != ESP_OK) {
        return send_error_response(req, 400, "Invalid JSON");
    }
    
    cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
    cJSON *endpoint_ids = cJSON_GetObjectItem(json, "endpoint_ids");
    cJSON *cluster_ids = cJSON_GetObjectItem(json, "cluster_ids");
    cJSON *event_ids = cJSON_GetObjectItem(json, "event_ids");
    
    if (!node_id || !endpoint_ids || !cluster_ids || !event_ids ||
        !cJSON_IsString(node_id) || !cJSON_IsString(endpoint_ids) ||
        !cJSON_IsString(cluster_ids) || !cJSON_IsString(event_ids) ||
        !node_id->valuestring || !endpoint_ids->valuestring ||
        !cluster_ids->valuestring || !event_ids->valuestring) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Missing or invalid required parameters");
    }
    
    uint64_t nodeId = string_to_uint64(node_id->valuestring);
    
    ScopedMemoryBufferWithSize<uint16_t> ep_ids;
    ScopedMemoryBufferWithSize<uint32_t> cl_ids;
    ScopedMemoryBufferWithSize<uint32_t> ev_ids;
    
    esp_err_t result = ESP_FAIL;
    
    // Pre-validate and allocate all arrays before acquiring lock
    if (string_to_uint16_array(endpoint_ids->valuestring, ep_ids) != ESP_OK) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Invalid endpoint_ids format");
    }
    
    if (string_to_uint32_array(cluster_ids->valuestring, cl_ids) != ESP_OK) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Invalid cluster_ids format");
    }
    
    if (string_to_uint32_array(event_ids->valuestring, ev_ids) != ESP_OK) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Invalid event_ids format");
    }
    
    // Lock the Matter stack before calling read event command
    esp_matter::lock::status_t lock_status = esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (lock_status != esp_matter::lock::SUCCESS) {
        ESP_LOGE(TAG, "Failed to acquire Matter stack lock");
        cJSON_Delete(json);
        return send_error_response(req, 500, "Internal server error - failed to acquire lock");
    }
    
    result = controller::send_read_event_command(nodeId, ep_ids, cl_ids, ev_ids);
    esp_matter::lock::chip_stack_unlock();
    
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        cJSON_Delete(json);
        return send_error_response(req, 500, "Failed to create response");
    }
    
    if (result == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Read event command sent successfully");
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Failed to send read event command");
    }
    
    ret = send_json_response(req, response, result == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ret;
}

// API: POST /api/subscribe-attribute - Subscribe to attributes
esp_err_t subscribe_attribute_handler(httpd_req_t *req) {
    cJSON *json = NULL;
    esp_err_t ret = parse_json_request(req, &json);
    if (ret != ESP_OK) {
        return send_error_response(req, 400, "Invalid JSON");
    }
    
    cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
    cJSON *endpoint_ids = cJSON_GetObjectItem(json, "endpoint_ids");
    cJSON *cluster_ids = cJSON_GetObjectItem(json, "cluster_ids");
    cJSON *attribute_ids = cJSON_GetObjectItem(json, "attribute_ids");
    cJSON *min_interval = cJSON_GetObjectItem(json, "min_interval");
    cJSON *max_interval = cJSON_GetObjectItem(json, "max_interval");
    
    if (!node_id || !endpoint_ids || !cluster_ids || !attribute_ids || !min_interval || !max_interval ||
        !cJSON_IsString(node_id) || !cJSON_IsString(endpoint_ids) ||
        !cJSON_IsString(cluster_ids) || !cJSON_IsString(attribute_ids) ||
        !cJSON_IsString(min_interval) || !cJSON_IsString(max_interval) ||
        !node_id->valuestring || !endpoint_ids->valuestring ||
        !cluster_ids->valuestring || !attribute_ids->valuestring ||
        !min_interval->valuestring || !max_interval->valuestring) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Missing or invalid required parameters");
    }
    
    uint64_t nodeId = string_to_uint64(node_id->valuestring);
    uint16_t minInterval = string_to_uint16(min_interval->valuestring);
    uint16_t maxInterval = string_to_uint16(max_interval->valuestring);
    
    ScopedMemoryBufferWithSize<uint16_t> ep_ids;
    ScopedMemoryBufferWithSize<uint32_t> cl_ids;
    ScopedMemoryBufferWithSize<uint32_t> attr_ids;
    
    esp_err_t result = ESP_FAIL;
    
    // Pre-validate and allocate all arrays before acquiring lock
    if (string_to_uint16_array(endpoint_ids->valuestring, ep_ids) != ESP_OK) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Invalid endpoint_ids format");
    }
    
    if (string_to_uint32_array(cluster_ids->valuestring, cl_ids) != ESP_OK) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Invalid cluster_ids format");
    }
    
    if (string_to_uint32_array(attribute_ids->valuestring, attr_ids) != ESP_OK) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Invalid attribute_ids format");
    }
    
    // Lock the Matter stack before calling subscribe attribute command
    esp_matter::lock::status_t lock_status = esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (lock_status != esp_matter::lock::SUCCESS) {
        ESP_LOGE(TAG, "Failed to acquire Matter stack lock");
        cJSON_Delete(json);
        return send_error_response(req, 500, "Internal server error - failed to acquire lock");
    }
    
    result = controller::send_subscribe_attr_command(nodeId, ep_ids, cl_ids, attr_ids, minInterval, maxInterval);
    esp_matter::lock::chip_stack_unlock();
    
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        cJSON_Delete(json);
        return send_error_response(req, 500, "Failed to create response");
    }
    if (result == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Subscribe attribute command sent successfully");
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Failed to send subscribe attribute command");
    }
    
    ret = send_json_response(req, response, result == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ret;
}

// API: POST /api/subscribe-event - Subscribe to events
esp_err_t subscribe_event_handler(httpd_req_t *req) {
    cJSON *json = NULL;
    esp_err_t ret = parse_json_request(req, &json);
    if (ret != ESP_OK) {
        return send_error_response(req, 400, "Invalid JSON");
    }
    
    cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
    cJSON *endpoint_ids = cJSON_GetObjectItem(json, "endpoint_ids");
    cJSON *cluster_ids = cJSON_GetObjectItem(json, "cluster_ids");
    cJSON *event_ids = cJSON_GetObjectItem(json, "event_ids");
    cJSON *min_interval = cJSON_GetObjectItem(json, "min_interval");
    cJSON *max_interval = cJSON_GetObjectItem(json, "max_interval");
    
    if (!node_id || !endpoint_ids || !cluster_ids || !event_ids || !min_interval || !max_interval ||
        !cJSON_IsString(node_id) || !cJSON_IsString(endpoint_ids) ||
        !cJSON_IsString(cluster_ids) || !cJSON_IsString(event_ids) ||
        !cJSON_IsString(min_interval) || !cJSON_IsString(max_interval) ||
        !node_id->valuestring || !endpoint_ids->valuestring ||
        !cluster_ids->valuestring || !event_ids->valuestring ||
        !min_interval->valuestring || !max_interval->valuestring) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Missing or invalid required parameters");
    }
    
    uint64_t nodeId = string_to_uint64(node_id->valuestring);
    uint16_t minInterval = string_to_uint16(min_interval->valuestring);
    uint16_t maxInterval = string_to_uint16(max_interval->valuestring);
    
    ScopedMemoryBufferWithSize<uint16_t> ep_ids;
    ScopedMemoryBufferWithSize<uint32_t> cl_ids;
    ScopedMemoryBufferWithSize<uint32_t> ev_ids;
    
    esp_err_t result = ESP_FAIL;
    
    // Pre-validate and allocate all arrays before acquiring lock
    if (string_to_uint16_array(endpoint_ids->valuestring, ep_ids) != ESP_OK) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Invalid endpoint_ids format");
    }
    
    if (string_to_uint32_array(cluster_ids->valuestring, cl_ids) != ESP_OK) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Invalid cluster_ids format");
    }
    
    if (string_to_uint32_array(event_ids->valuestring, ev_ids) != ESP_OK) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Invalid event_ids format");
    }
    
    // Lock the Matter stack before calling subscribe event command
    esp_matter::lock::status_t lock_status = esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (lock_status != esp_matter::lock::SUCCESS) {
        ESP_LOGE(TAG, "Failed to acquire Matter stack lock");
        cJSON_Delete(json);
        return send_error_response(req, 500, "Internal server error - failed to acquire lock");
    }
    
    result = controller::send_subscribe_event_command(nodeId, ep_ids, cl_ids, ev_ids, minInterval, maxInterval);
    esp_matter::lock::chip_stack_unlock();
    
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        cJSON_Delete(json);
        return send_error_response(req, 500, "Failed to create response");
    }
    if (result == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Subscribe event command sent successfully");
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Failed to send subscribe event command");
    }
    
    ret = send_json_response(req, response, result == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ret;
}

// API: POST /api/shutdown-subscription - Shutdown specific subscription
esp_err_t shutdown_subscription_handler(httpd_req_t *req) {
    cJSON *json = NULL;
    esp_err_t ret = parse_json_request(req, &json);
    if (ret != ESP_OK) {
        return send_error_response(req, 400, "Invalid JSON");
    }
    
    cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
    cJSON *subscription_id = cJSON_GetObjectItem(json, "subscription_id");
    
    if (!node_id || !subscription_id || !cJSON_IsString(node_id) || !cJSON_IsString(subscription_id) ||
        !node_id->valuestring || !subscription_id->valuestring) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Missing or invalid node_id or subscription_id");
    }
    
    uint64_t nodeId = string_to_uint64(node_id->valuestring);
    uint32_t subId = string_to_uint32(subscription_id->valuestring);
    
    // Lock the Matter stack before calling shutdown subscription command
    esp_matter::lock::status_t lock_status = esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (lock_status != esp_matter::lock::SUCCESS) {
        ESP_LOGE(TAG, "Failed to acquire Matter stack lock");
        cJSON_Delete(json);
        return send_error_response(req, 500, "Internal server error - failed to acquire lock");
    }
    
    esp_err_t result = controller::send_shutdown_subscription(nodeId, subId);
    esp_matter::lock::chip_stack_unlock();
    
    cJSON *response = cJSON_CreateObject();
    if (result == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Subscription shutdown successfully");
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Failed to shutdown subscription");
    }
    
    ret = send_json_response(req, response, result == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ret;
}

// API: POST /api/shutdown-all-subscriptions - Shutdown all subscriptions
esp_err_t shutdown_all_subscriptions_handler(httpd_req_t *req) {
    cJSON *json = NULL;
    esp_err_t ret = parse_json_request(req, &json);
    if (ret != ESP_OK) {
        return send_error_response(req, 400, "Invalid JSON");
    }
    
    cJSON *node_id = cJSON_GetObjectItem(json, "node_id");
    
    // Lock the Matter stack before calling shutdown subscriptions command
    esp_matter::lock::status_t lock_status = esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (lock_status != esp_matter::lock::SUCCESS) {
        ESP_LOGE(TAG, "Failed to acquire Matter stack lock");
        cJSON_Delete(json);
        return send_error_response(req, 500, "Internal server error - failed to acquire lock");
    }
    
    if (node_id) {
        // Shutdown subscriptions for specific node
        if (!cJSON_IsString(node_id) || !node_id->valuestring) {
            esp_matter::lock::chip_stack_unlock();
            cJSON_Delete(json);
            return send_error_response(req, 400, "Invalid node_id parameter");
        }
        uint64_t nodeId = string_to_uint64(node_id->valuestring);
        controller::send_shutdown_subscriptions(nodeId);
    } else {
        // Shutdown all subscriptions
        controller::send_shutdown_all_subscriptions();
    }
    esp_matter::lock::chip_stack_unlock();
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "message", "All subscriptions shutdown successfully");
    
    ret = send_json_response(req, response, 200);
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ret;
}

#if CONFIG_ENABLE_ESP32_CONTROLLER_BLE_SCAN
// API: POST /api/ble-scan - BLE scan
esp_err_t ble_scan_handler(httpd_req_t *req) {
    cJSON *json = NULL;
    esp_err_t ret = parse_json_request(req, &json);
    if (ret != ESP_OK) {
        return send_error_response(req, 400, "Invalid JSON");
    }
    
    cJSON *timeout = cJSON_GetObjectItem(json, "timeout");
    cJSON *details = cJSON_GetObjectItem(json, "details");
    
    if (!timeout || !cJSON_IsNumber(timeout)) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Missing or invalid timeout parameter");
    }
    
    uint16_t timeout_val = (uint16_t)timeout->valueint;
    if (timeout_val == 0 || timeout_val > 60) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Timeout must be between 1 and 60 seconds");
    }
    
    bool show_details = details && cJSON_IsTrue(details);
    
    // Create callback instance
    static controller::ble_scan::ConsoleBLEScanCallback callback;
    callback.SetShowDetails(show_details);
    
    // Lock the Matter stack before calling BLE scan commands
    esp_matter::lock::status_t lock_status = esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (lock_status != esp_matter::lock::SUCCESS) {
        ESP_LOGE(TAG, "Failed to acquire Matter stack lock");
        cJSON_Delete(json);
        return send_error_response(req, 500, "Internal server error - failed to acquire lock");
    }
    
    // Start enhanced scanning
    auto& scanner = controller::ble_scan::EnhancedBLEDeviceScanner::GetInstance();
    if (scanner.IsScanning()) {
        ESP_LOGW(TAG, "BLE scan already in progress. Stopping previous scan...");
        scanner.StopScan();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    esp_err_t result = scanner.StartScan(timeout_val, &callback);
    esp_matter::lock::chip_stack_unlock();
    
    cJSON *response = cJSON_CreateObject();
    if (result == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "BLE scan started successfully");
        cJSON_AddNumberToObject(response, "timeout", timeout_val);
        cJSON_AddBoolToObject(response, "show_details", show_details);
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Failed to start BLE scan");
    }
    
    ret = send_json_response(req, response, result == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ret;
}
#endif

// API: POST /api/group-settings - Group settings management
esp_err_t group_settings_handler(httpd_req_t *req) {
#ifndef CONFIG_ESP_MATTER_ENABLE_MATTER_SERVER
    cJSON *json = NULL;
    esp_err_t ret = parse_json_request(req, &json);
    if (ret != ESP_OK) {
        return send_error_response(req, 400, "Invalid JSON");
    }
    
    cJSON *action = cJSON_GetObjectItem(json, "action");
    if (!action || !cJSON_IsString(action)) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Missing or invalid 'action' field");
    }
    
    esp_err_t result = ESP_FAIL;
    cJSON *response = cJSON_CreateObject();
    
    // Lock the Matter stack before calling group settings commands
    esp_matter::lock::status_t lock_status = esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (lock_status != esp_matter::lock::SUCCESS) {
        ESP_LOGE(TAG, "Failed to acquire Matter stack lock");
        cJSON_Delete(json);
        cJSON_Delete(response);
        return send_error_response(req, 500, "Internal server error - failed to acquire lock");
    }
    
    if (strcmp(action->valuestring, "show-groups") == 0) {
        result = controller::group_settings::show_groups();
    } else if (strcmp(action->valuestring, "add-group") == 0) {
        cJSON *group_id = cJSON_GetObjectItem(json, "group_id");
        cJSON *group_name = cJSON_GetObjectItem(json, "group_name");
        
        if (!group_id || !group_name) {
            esp_matter::lock::chip_stack_unlock();
            cJSON_Delete(json);
            cJSON_Delete(response);
            return send_error_response(req, 400, "Missing group_id or group_name");
        }
        
        uint16_t groupId = string_to_uint16(group_id->valuestring);
        result = controller::group_settings::add_group(group_name->valuestring, groupId);
    } else if (strcmp(action->valuestring, "remove-group") == 0) {
        cJSON *group_id = cJSON_GetObjectItem(json, "group_id");
        
        if (!group_id) {
            esp_matter::lock::chip_stack_unlock();
            cJSON_Delete(json);
            cJSON_Delete(response);
            return send_error_response(req, 400, "Missing group_id");
        }
        
        uint16_t groupId = string_to_uint16(group_id->valuestring);
        result = controller::group_settings::remove_group(groupId);
    } else {
        esp_matter::lock::chip_stack_unlock();
        cJSON_Delete(json);
        cJSON_Delete(response);
        return send_error_response(req, 400, "Unsupported action");
    }
    esp_matter::lock::chip_stack_unlock();
    
    if (result == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Group settings command executed successfully");
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Group settings command failed");
    }
    
    ret = send_json_response(req, response, result == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ret;
#else
    return send_error_response(req, 400, "Group settings not available when Matter server is enabled");
#endif
}

// API: POST /api/udc - UDC commands
esp_err_t udc_handler(httpd_req_t *req) {
#if CONFIG_ESP_MATTER_COMMISSIONER_ENABLE && CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY
    cJSON *json = NULL;
    esp_err_t ret = parse_json_request(req, &json);
    if (ret != ESP_OK) {
        return send_error_response(req, 400, "Invalid JSON");
    }
    
    cJSON *action = cJSON_GetObjectItem(json, "action");
    if (!action || !cJSON_IsString(action)) {
        cJSON_Delete(json);
        return send_error_response(req, 400, "Missing or invalid 'action' field");
    }
    
    esp_err_t result = ESP_OK;
    cJSON *response = cJSON_CreateObject();
    
    // Lock the Matter stack before calling UDC commands
    esp_matter::lock::status_t lock_status = esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    if (lock_status != esp_matter::lock::SUCCESS) {
        ESP_LOGE(TAG, "Failed to acquire Matter stack lock");
        cJSON_Delete(json);
        cJSON_Delete(response);
        return send_error_response(req, 500, "Internal server error - failed to acquire lock");
    }
    
    if (strcmp(action->valuestring, "reset") == 0) {
        controller::matter_controller_client::get_instance()
            .get_commissioner()
            ->GetUserDirectedCommissioningServer()
            ->ResetUDCClientProcessingStates();
    } else if (strcmp(action->valuestring, "print") == 0) {
        controller::matter_controller_client::get_instance()
            .get_commissioner()
            ->GetUserDirectedCommissioningServer()
            ->PrintUDCClients();
    } else if (strcmp(action->valuestring, "commission") == 0) {
        cJSON *pincode = cJSON_GetObjectItem(json, "pincode");
        cJSON *index = cJSON_GetObjectItem(json, "index");
        
        if (!pincode || !index) {
            esp_matter::lock::chip_stack_unlock();
            cJSON_Delete(json);
            cJSON_Delete(response);
            return send_error_response(req, 400, "Missing pincode or index");
        }
        
        uint32_t pin = string_to_uint32(pincode->valuestring);
        size_t idx = (size_t)string_to_uint32(index->valuestring);
        
        controller::matter_controller_client &instance = controller::matter_controller_client::get_instance();
        UDCClientState *state = instance.get_commissioner()->GetUserDirectedCommissioningServer()->GetUDCClients().GetUDCClientState(idx);
        
        if (state != nullptr) {
            state->SetUDCClientProcessingState(chip::Protocols::UserDirectedCommissioning::UDCClientProcessingState::kCommissioningNode);
            
            chip::NodeId gRemoteId = chip::kTestDeviceNodeId;
            chip::RendezvousParameters params = chip::RendezvousParameters()
                                                    .SetSetupPINCode(pin)
                                                    .SetDiscriminator(state->GetLongDiscriminator())
                                                    .SetPeerAddress(state->GetPeerAddress());
            do {
                chip::Crypto::DRBG_get_bytes(reinterpret_cast<uint8_t *>(&gRemoteId), sizeof(gRemoteId));
            } while (!chip::IsOperationalNodeId(gRemoteId));
            
            if (instance.get_commissioner()->PairDevice(gRemoteId, params) != CHIP_NO_ERROR) {
                result = ESP_FAIL;
            }
        } else {
            result = ESP_FAIL;
        }
    } else {
        esp_matter::lock::chip_stack_unlock();
        cJSON_Delete(json);
        cJSON_Delete(response);
        return send_error_response(req, 400, "Unsupported UDC action");
    }
    esp_matter::lock::chip_stack_unlock();
    
    if (result == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "UDC command executed successfully");
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "UDC command failed");
    }
    
    ret = send_json_response(req, response, result == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    cJSON_Delete(response);
    return ret;
#else
    return send_error_response(req, 400, "UDC not available - Commissioner discovery not enabled");
#endif
}

// HTTP Server management functions
esp_err_t start_http_server(const http_server_config_t *config) {
    if (s_server != NULL) {
        ESP_LOGW(TAG, "HTTP server already started");
        return ESP_ERR_INVALID_STATE;
    }
    
    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = config->port;
    httpd_config.max_uri_handlers = config->max_uri_handlers;
    httpd_config.max_resp_headers = config->max_resp_headers;
    httpd_config.max_open_sockets = config->max_open_sockets;
    httpd_config.lru_purge_enable = true;
    
    // Increase stack size for HTTP server task to prevent stack overflow
    httpd_config.stack_size = 12288; // 12KB instead of default 4KB
    
    // Increase receive timeout to handle slow Matter operations
    httpd_config.recv_wait_timeout = 10; // 10 seconds
    httpd_config.send_wait_timeout = 10; // 10 seconds
    
    // Enable URI match wildcard to handle CORS OPTIONS requests
    httpd_config.uri_match_fn = httpd_uri_match_wildcard;
    
    s_cors_enabled = config->cors_enable;
    
    esp_err_t ret = httpd_start(&s_server, &httpd_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error starting HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Register URI handlers
    httpd_uri_t uri_handlers[] = {
        {
            .uri = "/api/help",
            .method = HTTP_GET,
            .handler = help_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/pairing",
            .method = HTTP_POST,
            .handler = pairing_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/group-settings",
            .method = HTTP_POST,
            .handler = group_settings_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/udc",
            .method = HTTP_POST,
            .handler = udc_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/open-commissioning-window",
            .method = HTTP_POST,
            .handler = open_commissioning_window_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/invoke-command",
            .method = HTTP_POST,
            .handler = invoke_command_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/read-attribute",
            .method = HTTP_POST,
            .handler = read_attribute_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/write-attribute",
            .method = HTTP_POST,
            .handler = write_attribute_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/read-event",
            .method = HTTP_POST,
            .handler = read_event_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/subscribe-attribute",
            .method = HTTP_POST,
            .handler = subscribe_attribute_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/subscribe-event",
            .method = HTTP_POST,
            .handler = subscribe_event_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/shutdown-subscription",
            .method = HTTP_POST,
            .handler = shutdown_subscription_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/shutdown-all-subscriptions",
            .method = HTTP_POST,
            .handler = shutdown_all_subscriptions_handler,
            .user_ctx = NULL
        },
#if CONFIG_ENABLE_ESP32_CONTROLLER_BLE_SCAN
        {
            .uri = "/api/ble-scan",
            .method = HTTP_POST,
            .handler = ble_scan_handler,
            .user_ctx = NULL
        },
#endif
        // OPTIONS handler for CORS
        {
            .uri = "*",
            .method = HTTP_OPTIONS,
            .handler = options_handler,
            .user_ctx = NULL
        }
    };
    
    for (int i = 0; i < sizeof(uri_handlers) / sizeof(uri_handlers[0]); i++) {
        ret = httpd_register_uri_handler(s_server, &uri_handlers[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error registering URI handler: %s", esp_err_to_name(ret));
            httpd_stop(s_server);
            s_server = NULL;
            return ret;
        }
    }
    
    ESP_LOGI(TAG, "HTTP server started on port %d", config->port);
    return ESP_OK;
}

esp_err_t stop_http_server(void) {
    if (s_server == NULL) {
        ESP_LOGW(TAG, "HTTP server not started");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = httpd_stop(s_server);
    if (ret == ESP_OK) {
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    } else {
        ESP_LOGE(TAG, "Error stopping HTTP server: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

httpd_handle_t get_http_server_handle(void) {
    return s_server;
}

bool is_http_server_running(void) {
    return s_server != NULL;
}

} // namespace http_server
} // namespace controller
} // namespace esp_matter