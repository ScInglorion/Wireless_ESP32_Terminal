#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

/*Definitions*/
#define WIFI_SUCCESS 1 << 0
#define WIFI_FAILURE 1 << 1
#define TCP_SUCCESS 1 << 0
#define TCP_FAILURE 1 << 1
#define MAX_FAILURES 10
#define SSID "Terminal_AP"
#define PASS "super-strong-password"
#define CHANNEL 11
#define MAX_DEV 4
#define PORT 12345

#define KEEPALIVE_IDLE              1
#define KEEPALIVE_INTERVAL          1
#define KEEPALIVE_COUNT             1

/*Globals*/
// Tags
static const char*WI_TAG  = "Wifi";
static const char *TCP_TAG  = "TCP";

static EventGroupHandle_t wifi_event_group;

// AP event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(WI_TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(WI_TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}


void wifi_init_softap(void)
{
    int status = WIFI_FAILURE;

    //initialize the esp network interface
    ESP_ERROR_CHECK(esp_netif_init());

    //initialize default esp event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create AP
    esp_netif_create_default_wifi_ap();

    //setup wifi AP with the default wifi configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // wifi_event_group = xEventGroupCreate();
    // esp_event_handler_instance_t wifi_handler_event_instance;
   
    // checking for any wifi event with any wifi event type (connect/disconnect), and if it happens call wifi handler
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = SSID,
            .ssid_len = strlen(SSID),
            .channel = CHANNEL,
            .password = PASS,
            .max_connection = MAX_DEV,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                    .required = true,
            },
        },
    };
    if (strlen(PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(WI_TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             SSID, PASS, CHANNEL);


    //     EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
    //         WIFI_SUCCESS | WIFI_FAILURE,
    //         pdFALSE,
    //         pdFALSE,
    //         portMAX_DELAY);

    // if (bits & WIFI_SUCCESS) {
    //     ESP_LOGI(WI_TAG, "AP createrd");
    //     status = WIFI_SUCCESS;
    // } else if (bits & WIFI_FAILURE) {
    //     ESP_LOGI(WI_TAG, "Failed to create AP");
    //     status = WIFI_FAILURE;
    // } else {
    //     ESP_LOGE(WI_TAG, "UNEXPECTED EVENT");
    //     status = WIFI_FAILURE;
    // }         
   // return status;
}



void app_main(void)
{
    esp_err_t status = WIFI_FAILURE;
    esp_err_t storage = nvs_flash_init();
    if (storage == ESP_ERR_NVS_NO_FREE_PAGES || storage == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      storage = nvs_flash_init();
    }
    ESP_ERROR_CHECK(storage);
    wifi_init_softap();
    // if(status != WIFI_SUCCESS){
    //     ESP_LOGI(WI_TAG, "AP creation failed");
    //     return;
    // }



}
