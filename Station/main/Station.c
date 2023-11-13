#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

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
#define PORT 12345
#define AP_IP "192.168.4.1"
/*globals*/

// event group to contain status information
static EventGroupHandle_t wifi_event_group;

// number of retires 
static int wifi_try_no = 0;

// task tags
static const char *TAG_WI = "WIFI";
static const char *TAG_TCP = "TCP";

// event handler for wifi events
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    // behaviour in case of connecting to Acces_point
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
	{
		ESP_LOGI(TAG_WI, "Establishing connection with AP");
		esp_wifi_connect();
    // behaviour in case of disconnecting from Acces_point    
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
	{
		if (wifi_try_no < MAX_FAILURES)
		{
			ESP_LOGI(TAG_WI, "Reconnecting to AP");
			ESP_ERROR_CHECK(esp_wifi_connect());
			wifi_try_no++;
		} else {
			xEventGroupSetBits(wifi_event_group, WIFI_FAILURE);
		}
	}
}

//event handler for ip events
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    // behaviour after receiving IP from Ap
	if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
	{
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_WI, "STA IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_try_no = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_SUCCESS);
    }

}

esp_err_t connect_wifi()
{
	int status = WIFI_FAILURE;

	//initialize the esp network interface
	ESP_ERROR_CHECK(esp_netif_init());

	//initialize default esp event loop
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	//create wifi station
	esp_netif_create_default_wifi_sta();

	//setup wifi station with the default wifi configuration
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Start of connection event loops
	wifi_event_group = xEventGroupCreate(); // <- output of wifi event lands here (fuction inits place where it lands)

    esp_event_handler_instance_t wifi_handler_event_instance;
    // checking for any wifi event with any wifi event type (connect/disconnect), and if it happens call wifi handler
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &wifi_handler_event_instance));

    esp_event_handler_instance_t got_ip_event_instance;
    // checking for any ip event and if it's getting ip, call ip event handler
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &ip_event_handler,
                                                        NULL,
                                                        &got_ip_event_instance));

    /** START THE WIFI DRIVER **/
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = SSID,
            .password = PASS,
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    // set the wifi controller to be a station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );

    // set the wifi config
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );

    // start the wifi driver
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG_WI, "STA initialization complete");

    /** NOW WE WAIT **/
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_SUCCESS | WIFI_FAILURE,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_SUCCESS) {
        ESP_LOGI(TAG_WI, "Connected to ap");
        status = WIFI_SUCCESS;
    } else if (bits & WIFI_FAILURE) {
        ESP_LOGI(TAG_WI, "Failed to connect to ap");
        status = WIFI_FAILURE;
    } else {
        ESP_LOGE(TAG_WI, "UNEXPECTED EVENT");
        status = WIFI_FAILURE;
    }

    /* The event wi ll not be processed after unregister */
    // ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_event_instance));
    // ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_event_instance));
    // vEventGroupDelete(wifi_event_group);

    return status;
}

// Connect to the socket of ap
esp_err_t socket_connection(void){
    struct sockaddr_in ap_info = {0};
    char buffer[1024] = {0};
    ap_info.sin_family = AF_INET;
    ap_info.sin_port = htons(PORT);
    // puting IP into address
    inet_pton(AF_INET, AP_IP, &ap_info.sin_addr);

    // socket creation
    int soc = socket(AF_INET, SOCK_STREAM, 0);
    if(soc < 0){
        ESP_LOGI(TAG_TCP, "Socket creation Failed");
        return TCP_FAILURE;
    }
    ESP_LOGI(TAG_TCP, "Socket created succesfully");

    // creating socket connection
    if(connect(soc, (struct sockaddr *)&ap_info, sizeof(ap_info)) != 0){
        ESP_LOGI(TAG_TCP, "Unable to to connect to %s", inet_ntoa(ap_info.sin_addr.s_addr));
        close(soc);
        return TCP_FAILURE;
    }
    ESP_LOGI(TAG_TCP, "Connected to TCP server");

    return TCP_SUCCESS;
}


void app_main(void)
{
esp_err_t status = WIFI_FAILURE;

// Initialize Non-volatile memory
esp_err_t storage = nvs_flash_init();
if(storage == ESP_ERR_NVS_NO_FREE_PAGES || storage == ESP_ERR_NVS_NEW_VERSION_FOUND){
    ESP_ERROR_CHECK(nvs_flash_erase());
    storage = nvs_flash_init();
}
ESP_ERROR_CHECK(storage);

// Connect to AP
status = connect_wifi();
if(status != WIFI_SUCCESS){
    ESP_LOGI(TAG_WI, "Failed to connect to AP");
    return;
}

// Connect to socket
status = socket_connection();
if(status != TCP_SUCCESS){
    ESP_LOGI(TAG_TCP, "Failed socket connection");
    return;
}
}
