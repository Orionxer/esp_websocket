#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAXIMUM_RETRY 10
#define VIDEO_UPLOAD_TRIGGER   "/upload_video"
#define VIDEO_UPLOAD_START     "/file_start"
#define VIDEO_UPLOAD_END       "/file_end"
#define VIDEO_UPLOAD_CHUNK_SIZE 4096
#define VIDEO_UPLOAD_TASK_STACK 4096
#define VIDEO_UPLOAD_TASK_PRIO  5
#define WEBSOCKET_TEXT_OPCODE   0x1
#define COMMAND_BUFFER_SIZE     64

static const char *TAG = "my_websocket";

static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_num;
static bool s_use_ssl;
static bool s_upload_in_progress;
static char s_command_buffer[COMMAND_BUFFER_SIZE];
static size_t s_command_length;
static size_t s_last_upload_size;
static int64_t s_upload_start_time_us;
static int64_t s_upload_end_time_us;
static int64_t s_file_end_sent_time_us;

extern const char AAA_Certificate_Services_pem_start[] asm("_binary_AAA_Certificate_Services_pem_start");
extern const char AAA_Certificate_Services_pem_end[] asm("_binary_AAA_Certificate_Services_pem_end");
extern const uint8_t video_start[] asm("_binary_starship_1mb_mp4_start");
extern const uint8_t video_end[] asm("_binary_starship_1mb_mp4_end");

static bool embedded_video_exists(size_t *file_size)
{
    size_t size = (size_t)(video_end - video_start);

    if (file_size != NULL) {
        *file_size = size;
    }

    return size > 0;
}

static bool embedded_video_sha256(char *hex_out, size_t hex_out_size)
{
    uint8_t digest[32];
    size_t video_size;

    if (hex_out == NULL || hex_out_size < 65) {
        return false;
    }

    video_size = (size_t)(video_end - video_start);
    if (video_size == 0) {
        return false;
    }

    mbedtls_sha256(video_start, video_size, digest, 0);

    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(&hex_out[i * 2], hex_out_size - (i * 2), "%02x", digest[i]);
    }

    return true;
}

static bool uri_uses_wss(const char *uri)
{
    return uri != NULL && strncmp(uri, "wss://", strlen("wss://")) == 0;
}

static void reset_command_buffer(void)
{
    s_command_length = 0;
    s_command_buffer[0] = '\0';
}

static void reset_upload_metrics(void)
{
    s_last_upload_size = 0;
    s_upload_start_time_us = 0;
    s_upload_end_time_us = 0;
    s_file_end_sent_time_us = 0;
}

static const char *websocket_hello_message(void)
{
    return s_use_ssl ? "hello from esp32c5 with ssl" : "hello from esp32c5";
}

static bool send_text_command(esp_websocket_client_handle_t client, const char *command)
{
    int command_len;
    int written;

    if (client == NULL || command == NULL) {
        return false;
    }

    command_len = (int)strlen(command);
    written = esp_websocket_client_send_text(client,
                                             command,
                                             command_len,
                                             pdMS_TO_TICKS(5000));
    if (written != command_len) {
        ESP_LOGE(TAG, "Failed to send text command %s, written=%d", command, written);
        return false;
    }

    ESP_LOGI(TAG, "Sent text command: %s", command);
    return true;
}

static void video_upload_task(void *pvParameter)
{
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)pvParameter;
    size_t video_size = (size_t)(video_end - video_start);
    size_t offset = 0;
    size_t chunk_index = 0;

    if (video_size == 0) {
        ESP_LOGE(TAG, "Embedded video is empty");
        goto cleanup;
    }

    if (!esp_websocket_client_is_connected(client)) {
        ESP_LOGE(TAG, "WebSocket is not connected, skip upload");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Starting video upload, size=%u bytes", (unsigned)video_size);
    if (!send_text_command(client, VIDEO_UPLOAD_START)) {
        goto cleanup;
    }
    s_last_upload_size = video_size;
    s_upload_start_time_us = esp_timer_get_time();
    s_upload_end_time_us = 0;
    s_file_end_sent_time_us = 0;

    while (offset < video_size) {
        size_t remaining = video_size - offset;
        size_t chunk_size = remaining > VIDEO_UPLOAD_CHUNK_SIZE ? VIDEO_UPLOAD_CHUNK_SIZE : remaining;
        int written;

        if (!esp_websocket_client_is_connected(client)) {
            ESP_LOGE(TAG, "WebSocket disconnected during upload");
            goto cleanup;
        }

        written = esp_websocket_client_send_bin(client,
                                                (const char *)(video_start + offset),
                                                (int)chunk_size,
                                                pdMS_TO_TICKS(10000));
        if (written != (int)chunk_size) {
            ESP_LOGE(TAG,
                     "Failed to send video chunk %u, offset=%u, expected=%u, written=%d",
                     (unsigned)chunk_index,
                     (unsigned)offset,
                     (unsigned)chunk_size,
                     written);
            goto cleanup;
        }

        offset += chunk_size;
        chunk_index++;

        if ((chunk_index % 32U) == 0U || offset == video_size) {
            unsigned progress_percent = (unsigned)((offset * 100U) / video_size);

            ESP_LOGI(TAG,
                     "Video upload progress: %u%% (%u/%u bytes)",
                     progress_percent,
                     (unsigned)offset,
                     (unsigned)video_size);
        }
    }

    if (!send_text_command(client, VIDEO_UPLOAD_END)) {
        goto cleanup;
    }

    s_upload_end_time_us = esp_timer_get_time();
    s_file_end_sent_time_us = s_upload_end_time_us;

    ESP_LOGI(TAG,
             "Video upload completed successfully, total time: %.2f s",
             (double)(s_upload_end_time_us - s_upload_start_time_us) / 1000000.0);

cleanup:
    s_upload_in_progress = false;
    vTaskDelete(NULL);
}

static void start_video_upload(esp_websocket_client_handle_t client)
{
    BaseType_t task_created;

    if (s_upload_in_progress) {
        ESP_LOGW(TAG, "Video upload already in progress, ignore duplicate trigger");
        return;
    }

    if (!esp_websocket_client_is_connected(client)) {
        ESP_LOGE(TAG, "WebSocket is not connected, cannot start upload");
        return;
    }

    s_upload_in_progress = true;
    task_created = xTaskCreate(video_upload_task,
                               "ws_video_upload",
                               VIDEO_UPLOAD_TASK_STACK,
                               client,
                               VIDEO_UPLOAD_TASK_PRIO,
                               NULL);
    if (task_created != pdPASS) {
        s_upload_in_progress = false;
        ESP_LOGE(TAG, "Failed to create video upload task");
        return;
    }

    ESP_LOGI(TAG, "Video upload task started");
}

static void handle_text_frame(esp_websocket_client_handle_t client,
                              const esp_websocket_event_data_t *data)
{
    if (data->payload_offset == 0) {
        reset_command_buffer();
    }

    if ((size_t)data->payload_offset != s_command_length) {
        ESP_LOGW(TAG,
                 "Unexpected text frame offset, expected=%u actual=%d",
                 (unsigned)s_command_length,
                 data->payload_offset);
        reset_command_buffer();
        return;
    }

    if (s_command_length + (size_t)data->data_len >= sizeof(s_command_buffer)) {
        ESP_LOGW(TAG, "Incoming text command is too long, len=%u", (unsigned)data->payload_len);
        reset_command_buffer();
        return;
    }

    memcpy(&s_command_buffer[s_command_length], data->data_ptr, (size_t)data->data_len);
    s_command_length += (size_t)data->data_len;
    s_command_buffer[s_command_length] = '\0';

    if (data->payload_offset + data->data_len < data->payload_len) {
        return;
    }

    ESP_LOGI(TAG, "Received text frame: %s", s_command_buffer);
    if (strcmp(s_command_buffer, VIDEO_UPLOAD_TRIGGER) == 0) {
        start_video_upload(client);
    } else if (strcmp(s_command_buffer, "/file_received") == 0) {
        double upload_seconds = 0.0;
        double rtt_ms = 0.0;
        double bandwidth_kib_per_sec = 0.0;
        double bandwidth_mib_per_sec = 0.0;
        int64_t ack_time_us = esp_timer_get_time();

        if (s_upload_start_time_us > 0 && s_upload_end_time_us >= s_upload_start_time_us) {
            upload_seconds = (double)(s_upload_end_time_us - s_upload_start_time_us) / 1000000.0;
        }
        if (s_file_end_sent_time_us > 0 && ack_time_us >= s_file_end_sent_time_us) {
            rtt_ms = (double)(ack_time_us - s_file_end_sent_time_us) / 1000.0;
        }
        if (upload_seconds > 0.0 && s_last_upload_size > 0U) {
            bandwidth_kib_per_sec = ((double)s_last_upload_size / 1024.0) / upload_seconds;
            bandwidth_mib_per_sec = ((double)s_last_upload_size / (1024.0 * 1024.0)) / upload_seconds;
        }

        ESP_LOGI(TAG,
                 "Server confirmed file receipt, RTT: %.2f ms, bandwidth: %.2f KiB/s (%.2f MiB/s)",
                 rtt_ms,
                 bandwidth_kib_per_sec,
                 bandwidth_mib_per_sec);
    }

    reset_command_buffer();
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_num < WIFI_MAXIMUM_RETRY) {
            s_wifi_retry_num++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retrying (%d/%d)", s_wifi_retry_num, WIFI_MAXIMUM_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_retry_num = 0;
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void websocket_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected over %s: %s", s_use_ssl ? "WSS" : "WS", CONFIG_MY_WEBSOCKET_URI);
        if (!send_text_command(client, websocket_hello_message())) {
            ESP_LOGE(TAG, "Failed to send initial hello message after connect");
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == WEBSOCKET_TEXT_OPCODE) {
            handle_text_frame(client, data);
        } else {
            if (data->op_code == 0x8U) {
                ESP_LOGW(TAG,
                         "Received WebSocket close frame (opcode=%" PRIu8 ", len=%d, total=%d, offset=%d)",
                         data->op_code,
                         data->data_len,
                         data->payload_len,
                         data->payload_offset);
            }
            ESP_LOGI(TAG,
                     "Received non-text frame (opcode=%" PRIu8 ", len=%d, total=%d, offset=%d)",
                     data->op_code,
                     data->data_len,
                     data->payload_len,
                     data->payload_offset);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_upload_in_progress = false;
        reset_command_buffer();
        reset_upload_metrics();
        ESP_LOGW(TAG, "WebSocket disconnected");
        if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "Transport errno=%d, tls_err=0x%x, stack_err=0x%x",
                     data->error_handle.esp_transport_sock_errno,
                     data->error_handle.esp_tls_last_esp_err,
                     data->error_handle.esp_tls_stack_err);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error, handshake_status=%d",
                 data->error_handle.esp_ws_handshake_status_code);
        break;
    default:
        break;
    }

    (void)base;
}

static void wifi_init_sta(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .sae_h2e_identifier = "",
        },
    };
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    EventBits_t bits;

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return;
    }

    strncpy((char *)wifi_config.sta.ssid, CONFIG_MY_WEBSOCKET_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, CONFIG_MY_WEBSOCKET_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s", CONFIG_MY_WEBSOCKET_WIFI_SSID);
    bits = xEventGroupWaitBits(s_wifi_event_group,
                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                               pdFALSE,
                               pdFALSE,
                               pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi ready");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi after %d retries", WIFI_MAXIMUM_RETRY);
    } else {
        ESP_LOGE(TAG, "Timed out waiting for Wi-Fi connection");
    }
}

static void websocket_start(void)
{
    const esp_websocket_client_config_t websocket_cfg = {
        .uri = CONFIG_MY_WEBSOCKET_URI,
        .cert_pem = s_use_ssl ? AAA_Certificate_Services_pem_start : NULL,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 5000,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&websocket_cfg);

    if (s_use_ssl) {
        ESP_LOGI(TAG, "Detected WSS URI, enabling TLS with embedded certificate (%d bytes)",
                 (int)(AAA_Certificate_Services_pem_end - AAA_Certificate_Services_pem_start));
    } else {
        ESP_LOGI(TAG, "Detected WS URI, starting without TLS certificate");
    }

    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return;
    }

    ESP_ERROR_CHECK(esp_websocket_register_events(client,
                                                  WEBSOCKET_EVENT_ANY,
                                                  websocket_event_handler,
                                                  client));
    ESP_ERROR_CHECK(esp_websocket_client_start(client));
    ESP_LOGI(TAG, "WebSocket client started, target=%s", CONFIG_MY_WEBSOCKET_URI);
}

void app_main(void)
{
    esp_err_t ret;
    size_t video_size = 0;
    char video_sha256_hex[65];
    ESP_LOGI(TAG, "Starting WebSocket demo");
    s_use_ssl = uri_uses_wss(CONFIG_MY_WEBSOCKET_URI);
    reset_command_buffer();
    reset_upload_metrics();
    if (embedded_video_exists(&video_size)) {
        ESP_LOGI(TAG, "Video exists, size=%u bytes", (unsigned)video_size);
        if (embedded_video_sha256(video_sha256_hex, sizeof(video_sha256_hex))) {
            ESP_LOGI(TAG, "Video SHA256=%s", video_sha256_hex);
        } else {
            ESP_LOGE(TAG, "Failed to calculate video SHA256");
        }
    } else {
        ESP_LOGE(TAG, "Video is missing");
    }

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();
    if (!(xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Wi-Fi is not connected, skip WebSocket connection");
        return;
    }

    websocket_start();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
