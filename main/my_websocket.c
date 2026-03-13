#include <inttypes.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAXIMUM_RETRY 10

static const char *TAG = "my_websocket";

static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_num;
static bool s_message_sent;

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
    case WEBSOCKET_EVENT_CONNECTED: {
        int len;

        ESP_LOGI(TAG, "WebSocket connected: %s", CONFIG_MY_WEBSOCKET_URI);
        if (s_message_sent) {
            return;
        }

        len = esp_websocket_client_send_text(client,
                                             CONFIG_MY_WEBSOCKET_MESSAGE,
                                             strlen(CONFIG_MY_WEBSOCKET_MESSAGE),
                                             pdMS_TO_TICKS(5000));
        if (len >= 0) {
            s_message_sent = true;
            ESP_LOGI(TAG, "Sent message: %s", CONFIG_MY_WEBSOCKET_MESSAGE);
        } else {
            ESP_LOGE(TAG, "Failed to send message");
        }
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        ESP_LOGI(TAG, "Received data (opcode=%" PRIu8 ", len=%d): %.*s",
                 data->op_code, data->data_len, data->data_len, data->data_ptr);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
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
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 5000,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&websocket_cfg);

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

    ESP_LOGI(TAG, "Starting WebSocket demo");

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
