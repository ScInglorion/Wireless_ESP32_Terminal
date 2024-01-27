#include <string.h>
#include <memory.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "lvgl.h"
#include "esp_lcd_ili9341.h"
#include "lwip/sockets.h"

/*Definitions*/
#define KEEPALIVE_IDLE              1
#define KEEPALIVE_INTERVAL          1
#define KEEPALIVE_COUNT             1
#define WIFI_SUCCESS 1 << 0
#define WIFI_FAILURE 1 << 1
#define TCP_SUCCESS 1 << 0
#define TCP_FAILURE 1 << 1
#define MAX_FAILURES 10
#define SSID "Terminal_AP" 
#define PASS "super-strong-password"
#define PORT 12345
#define AP_IP "192.168.4.1"
#define LVGL_TICK_PERIOD_MS 2
#define TFT_BK_LIGHT_ON 1
#define TFT_BK_LIGHT_OFF !TFT_BK_LIGHT_ON

// Defining SPI
#define LCD_HOST  SPI2_HOST

/*Pin definition*/
// Display
#define SCLK_PIN 18
#define MOSI_PIN 19
#define MISO_PIN 21
#define DC_PIN 5
#define RST_PIN 22
#define TFT_CS_PIN 4
#define BK_LIGHT_PIN 2

// Keypad
#define R1 13
#define R2 12
#define R3 14
#define R4 27
#define C1 26
#define C2 25
#define C3 33
#define C4 32

// Display specs
#define TFT_PIXEL_CLOCK_HZ (20 * 1000* 1000) // 20 MHZ
#define HOR_RES 320           
#define VER_RES 240         

// Bit number used to represent dispaly command and parameter
#define CMD_BITS 8
#define PARAM_BITS 8

// Keyboard variables
#define KEYPAD_DEBOUNCING 100   // < time in ms
#define KEYPAD_STACKSIZE  5

// Shouldnt do that but oh well
lv_obj_t *label3;
lv_obj_t *label4;

static u_int8_t repeat = 0; // states which of states of letter in button is used 
static u_int8_t spec_num = 0; // states which position was used last time button was used
static char word[255]; // stores letter inputed by keypad
static char frame[255];
static char placeholder[255]; // created here due to occasional stack overflow happening if created inside the function. Stores letters from word minus last position
static char received_data[250];
static u_int8_t position = 0; // stores the position of last character in word 
const char keypad[] = { 
    '1', '2', '3', '.',
    '4', '5', '6', '+',
    '7', '8', '9', 'C',
    '`', '0', '#', 'D'
};  

const char keypad1[] = { 
    'a', 'd', 'g', ',',
    'j', 'm', 'p', '-',
    's', 'v', 'y', 'C',
    '`', ' ', '#', 'D'
};  

const char keypad2[] = { 
    'b', 'e', 'h', '!',
    'k', 'n', 'q', '*',
    't', 'w', 'z', 'C',
    '`', '_', '#', 'D'
};  

const char keypad3[] = { 
    'c', 'f', 'i', '?',
    'l', 'o', 'r', '/',
    'u', 'x', '$', 'C',
    '`', '=', '#', 'D'
};  

static gpio_num_t _keypad_pins[8];

// Last isr time
time_t time_old_isr = 0;
// Pressed keys queue
QueueHandle_t keypad_queue;

// event group to contain status information
static EventGroupHandle_t wifi_event_group;

// number of retires 
static int wifi_try_no = 0;
int socket_status = -1;

// socket definition
int soc;
char buffer[1024];
char send_buffer[255];

// task tags
static const char *TAG_WI = "WIFI";
static const char *TAG_TCP = "TCP";
static const char *TFT_TAG = "Display";
static const char *KEYPAD_TAG = "Keypad";

/*Frame functions*/
unsigned char Calculate_Crc(char frameid, char framelength, const char *data, u_int8_t length){
    unsigned char crc = 0x5a;
    crc += frameid;
    crc += framelength;
    // starts from one due to position 0 being frameid
    for(u_int8_t i = 1; i < length; i++){
        crc += data[i];
    }
    return crc % 256;
}

unsigned char Calculate_Xor(char frameid, char framelength, const char *data, u_int8_t length){
    unsigned char crc_xor = 0x5a;
    crc_xor ^= frameid;
    crc_xor ^= framelength;
    for(u_int8_t i = 1; i < length; i++){
        crc_xor ^= data[i];
    }
    return crc_xor;

}   

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
    /**(worked before adding lvgl, gotta fix)*/  
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
	{
		if (wifi_try_no < MAX_FAILURES)
		{
			ESP_LOGI(TAG_WI, "Reconnecting to AP");
			esp_wifi_connect();
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
    return status;
}

// Connect to the socket of ap
esp_err_t socket_connection(void){
    struct sockaddr_in ap_info = {0};
    ap_info.sin_family = AF_INET;
    ap_info.sin_port = htons(PORT);
    // puting IP into address
    inet_pton(AF_INET, AP_IP, &ap_info.sin_addr);

    // socket creation
    soc = socket(AF_INET, SOCK_STREAM, 0);
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
    socket_status = 0;
    ESP_LOGI(TAG_TCP, "Connected to TCP server");
    return TCP_SUCCESS;
}

static void socket_read(lv_obj_t *display){    
    while(1){
        if(socket_status == 0){
            bzero(buffer, sizeof(buffer));
            bzero(received_data, sizeof(received_data));
            int r = read(soc, buffer, sizeof(buffer));
            if (r > 0){
                ESP_LOGI("socket", "%i", r);
                ESP_LOGI("socket", "%s", buffer);
                ESP_LOG_BUFFER_HEXDUMP("dump", buffer, r, ESP_LOG_INFO);
                switch(buffer[2]){ 
                    case 48: // wilgotnosc, do 99
                        strcpy(received_data, "Temperatura: ");
                        ESP_LOGI("ds", "%s",received_data);
                        for(int i = 13; i < r + 7; i++){
                            received_data[i] = buffer[i - 9];
                        }
                        if(r > 9 || (r == 9 && (buffer[4] > '1' || (buffer[4] == '1' && (buffer[5] > '0' || buffer[6] > '0'))))){
                            lv_label_set_text(label3, "0x03");
                        }
                        else{
                            lv_label_set_text(label3, "0x00");
                        }
                         
                        lv_label_set_text(display, received_data);                         
                        ESP_LOGI("sadge", "%s",received_data);
                        break; 
                    case 49: // tekst
                        strcpy(received_data, "Komunikat: ");
                        for(int i = 11; i < r + 5; i++){
                            received_data[i] = buffer[i - 7];
                        }
                        lv_label_set_text(label3, "0x00"); 
                        lv_label_set_text(display, received_data);                                                 
                        break;
                    case 50: // read_only  
                        strcpy(received_data, "Wilgotnosc: ");
  
                        for(int i = 12; i < r + 6; i++){
                            received_data[i] = buffer[i - 8];
                        }                            
                        if(r > 9 || (r == 9 && (buffer[4] > '1' || (buffer[4] == '1' && (buffer[5] > '0' || buffer[6] > '0'))))){
                            lv_label_set_text(label3, "0x03");
                        }
                        else{
                            lv_label_set_text(label3, "0x00");
                        } 
                        lv_label_set_text(display, received_data);                          
                        break;
                    default:
                        lv_label_set_text(label3, "0x01"); 
                        lv_label_set_text(display, "Nie rozpoznano FrameID"); 
                }              
            }        
        }
        else{
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

/* Display */
static bool lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    // copy a buffer's content to a specific area of the display
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void disRefresh(void *arg){
    while (1) {
        // raise the task priority of LVGL and/or reduce the handler period can improve the performance
        vTaskDelay(pdMS_TO_TICKS(10));
        // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
        lv_timer_handler();
    }   
}

/*Keypad*/
void intr_click_handler(void *args);

/**
 * Enable rows'pin pullup resistor, and isr. Prepares
 * keypad to read pressed row number.
 */
void turnon_rows()
{
    for(int i = 4; i < 8; i++) /// Columns
    {
        gpio_set_pull_mode(_keypad_pins[i], GPIO_PULLDOWN_ONLY);
    }
    for(int i = 0; i < 4; i++) /// Rows
    {
        gpio_set_pull_mode(_keypad_pins[i], GPIO_PULLUP_ONLY);
        gpio_intr_enable(_keypad_pins[i]);
    }
}

/**
 *  Enable columns'pin pullup resistor, and disable rows isr and pullup resistor.
 * Prepares keypad to read pressed column number. 
 */
void turnon_cols()
{
    for(int i = 0; i < 4; i++) /// Rows
    {
        gpio_intr_disable(_keypad_pins[i]);
        gpio_set_pull_mode(_keypad_pins[i], GPIO_PULLDOWN_ONLY);
    }
    for(int i = 4; i < 8; i++) /// Columns
    {
        gpio_set_pull_mode(_keypad_pins[i], GPIO_PULLUP_ONLY);
    }
}

esp_err_t keypad_initalize(gpio_num_t keypad_pins[8])
{
    memcpy(_keypad_pins, keypad_pins, 8*sizeof(gpio_num_t));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_install_isr_service(ESP_INTR_FLAG_EDGE));
    for(int i = 0; i < 4; i++) /// Rows
    {
        gpio_intr_disable(keypad_pins[i]);
        gpio_set_direction(keypad_pins[i], GPIO_MODE_INPUT);
        gpio_set_intr_type(keypad_pins[i], GPIO_INTR_NEGEDGE);
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_isr_handler_add(_keypad_pins[i], (void*)intr_click_handler, (void*)i));
        
    }
    for(int i = 4; i < 8; i++)
    {
        gpio_set_direction(keypad_pins[i], GPIO_MODE_INPUT);
    }

    keypad_queue = xQueueCreate(5, sizeof(char));
    if(keypad_queue == NULL)
        return ESP_ERR_NO_MEM;

    turnon_rows();

    return ESP_OK;
}

void intr_click_handler(void* args)
{
    int index = (int)(args);
    
    time_t time_now_isr = time(NULL);
    time_t time_isr = (time_now_isr - time_old_isr)*1000L;
    
    if(time_isr >= KEYPAD_DEBOUNCING)
    {
        turnon_cols();
        for(int j = 4; j < 8; j++)
        {
            if(!gpio_get_level(_keypad_pins[j]))
            {
            if(spec_num == index*4 + j - 4 && repeat == 0)
            {
                xQueueSendFromISR(keypad_queue, &keypad[index*4 + j - 4], NULL);
                spec_num = index*4 + j - 4;
                repeat = 1;
                break;                
            }            
            else if(spec_num == index*4 + j - 4 && repeat == 1)
            {
                xQueueSendFromISR(keypad_queue, &keypad1[index*4 + j - 4], NULL);
                spec_num = index*4 + j - 4;
                repeat = 2;
                break;                
            }
            else if(spec_num == index*4 + j - 4 && repeat == 2)
            {
                xQueueSendFromISR(keypad_queue, &keypad2[index*4 + j - 4], NULL);
                spec_num = index*4 + j - 4;
                repeat = 3;
                break;                
            }
            else if(spec_num == index*4 + j - 4 && repeat == 3)
            {
                xQueueSendFromISR(keypad_queue, &keypad3[index*4 + j - 4], NULL);
                spec_num = index*4 + j - 4;
                repeat = 0;
                break;                
            }
            else
            {
                xQueueSendFromISR(keypad_queue, &keypad[index*4 + j - 4], NULL);
                spec_num = index*4 + j - 4;
                repeat = 1;                                
                break;               
            }            
            }
        }
        turnon_rows();
    }
    time_old_isr = time_now_isr;
    
}

char keypad_getkey()
{
    char key;
    if(!uxQueueMessagesWaiting(keypad_queue)) /// if is empty, return teminator character
        return '\0';
    xQueueReceive(keypad_queue, &key, portMAX_DELAY);
    return key;
}

void keypad_delete()
{
    for(int i = 0; i < 8; i++)
    {   
        gpio_isr_handler_remove(_keypad_pins[i]);
        gpio_set_direction(_keypad_pins[i], GPIO_MODE_DISABLE);
    }
    vQueueDelete(keypad_queue);
}


static void keypadtask(lv_obj_t *txt){
    // spliting 16 bit number into 2 8bit numbers for prefix
    short frameid = 0xAA55;
    char frameid_bytes[2];
    frameid_bytes[0] = (frameid >> 8) & 0xFF;
    frameid_bytes[1] = frameid & 0xFF;
    static char safty_skip_flag = 'f';
    while(true)
    {
        char keypressed = keypad_getkey();  /// gets from key queue    
        
        if(keypressed != '\0' && keypressed != '`' && keypressed != 'D' && keypressed != 'C' && keypressed != '#'){ // Display character
            /* Pehaps add safty measure for array opverflow*/
            word[position] = keypressed;
            word[position + 1] = '\0';
            if(position == 0){
                frame[2] = keypressed;
            }
            else{
                frame[position + 3] = keypressed;
            }
            ESP_LOGI(KEYPAD_TAG, "Pressed key: %c\n", keypressed);
            ESP_LOGI(KEYPAD_TAG, "Pressed key: %s\n", word);
            lv_textarea_set_text(txt, word);
            safty_skip_flag = 't';
        }

        // clear the display
        else if (keypressed == '`'){
            ESP_LOGI(KEYPAD_TAG, "Pressed key: %c\n", keypressed);
            word[0] = ' ';
            word[1] = '\0';
            lv_textarea_set_text(txt, word);
            position = 0;
            safty_skip_flag = 'f';
            strcpy(frame, frameid_bytes);
        }
        else if (keypressed == 'D' && safty_skip_flag == 't'){ // going onto next character
            ESP_LOGI(KEYPAD_TAG, "Pressed key: %c\n", keypressed);
            position ++;
            safty_skip_flag = 'f';
            
        }
        else if (keypressed == 'C' && position > 0){ // delete last accepted character
            ESP_LOGI(KEYPAD_TAG, "Pressed key: %c\n", keypressed);
            for(int i=0; i < position; i++){
                placeholder[i] = word[i];
            }
            position --;
            for(int i=0; i < position; i++){
                word[i] = placeholder[i];
                if(i == 0){
                    frame[2] = placeholder[i];
                }
                else{
                    frame[3+i] = placeholder[i];
                }
            }
            word[position] = '\0';
            safty_skip_flag = 'f';
            lv_textarea_set_text(txt, word);
        }
        else if(keypressed == '#'){
            ESP_LOGI(KEYPAD_TAG, "Pressed key: %c\n", keypressed);
            if(position > 0){
                uint8_t* frame = (uint8_t*) malloc(position+6);
                frame[0] = frameid_bytes[0];
                frame[1] = frameid_bytes[1];
                frame[2] = word[0]; // at least for now, FrameId is the same as the first input of keypad
                for(u_int8_t i = 1; i < position+1; i++){
                    frame[3+i] = word[i];
                }
                if (frame[position+3] == 0x00){
                    frame[3] = position+5; // length of frame, 5 for fields besides data, position for data, as first number in data is for frameid for now (otherwise would be position +1), and we don't take into account the field with null value created by pressing D
                    frame[position + 3] = Calculate_Crc(frame[2], frame[3], word, position);
                    frame[position + 4] = Calculate_Xor(frame[2], frame[3], word, position);
                ESP_LOGI("frame", "%s", frame);
                ESP_LOG_BUFFER_HEXDUMP("dump", frame, position+5, ESP_LOG_INFO);
                }
                else{
                    frame[3] = position+6; // length of frame, 6 for fields besides data, position for data, as first number in data is for frameid for now (otherwise would be position +1)
                    frame[position + 4] = Calculate_Crc(frame[2], frame[3], word, position+1);
                    frame[position + 5] = Calculate_Xor(frame[2], frame[3], word, position+1);
                ESP_LOGI("frame", "%s", frame);
                ESP_LOG_BUFFER_HEXDUMP("dump", frame, position+6, ESP_LOG_INFO);
                }

                switch(frame[2]){ 
                    case '0': // temperatura do 100  if(r > 9 || (r == 9 && (buffer[4] > '1' || (buffer[4] == '1' && (buffer[5] > '0' || buffer[6] > '0')))))
                        if((position > 3 && word[position] != '\0') || ((position == 3 || position == 4) && (word[1] > '1' || (word[1] == '1' && (word[2] > '0' || word[3] > '0'))))){ // position > 2 && word[position] != '\0' jak byla wilgotnosc do 99
                            ESP_LOGI("Frame_Error", "ERROR 0x03");
                            lv_label_set_text(label4, "0x03");
                        }

                        else{ 
                            if(socket_status == 0){                  
                                ESP_LOGI("socket", "%s", frame);
                                write(soc, frame, position+6);          
                                ESP_LOGI("Frame_Error", "ERROR 0x00");
                                lv_label_set_text(label4, "0x00");            
                            }                            
                        }
                        break; 
                    case '1': // tekst
                       if(position > 249){
                            ESP_LOGI("Frame_Error", "ERROR 0x03");
                            lv_label_set_text(label4, "0x03");
                        }
                        else{ 
                            if(socket_status == 0){                  
                                ESP_LOGI("socket", "%s", frame);
                                write(soc, frame, position+6);          
                                ESP_LOGI("Frame_Error", "ERROR 0x00");
                                lv_label_set_text(label4, "0x00");            
                            }                            
                        }
                        break;
                    case '2': // read_only (wilgotnosc)
                        ESP_LOGI("Frame_Error", "ERROR 0x04");
                        lv_label_set_text(label4, "0x04");   
                        break;
                    default:
                        ESP_LOGI("Frame_Error", "ERROR 0x02");
                        lv_label_set_text(label4, "0x02");   
                }
                free(frame);
            }
     
            position = 0;
            safty_skip_flag = 'f';
        }        
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void display_initialize(){
    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer
    static lv_disp_drv_t disp_drv;      // contains callback functions

    ESP_LOGI(TFT_TAG, "Turn off LCD backlight");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << BK_LIGHT_PIN
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));    

    // Creating SPI bus
    ESP_LOGI(TFT_TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = SCLK_PIN,
        .mosi_io_num = MOSI_PIN,
        .miso_io_num = MISO_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = HOR_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
 
    // Allocating LCD IO device handle
    ESP_LOGI(TFT_TAG, "Installing IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = DC_PIN,
        .cs_gpio_num = TFT_CS_PIN,
        .pclk_hz = TFT_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = CMD_BITS,
        .lcd_param_bits = PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = lvgl_flush_ready,
        .user_ctx = &disp_drv,
    };
    
    // TFT attachment to SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    // Installing LCD controller drive
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = RST_PIN,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    // turing backlight on after initialization
    ESP_LOGI(TFT_TAG, "Turn on LCD backlight");
    gpio_set_level(BK_LIGHT_PIN, TFT_BK_LIGHT_ON);

    // Initialization of LVGL
    ESP_LOGI(TFT_TAG, "Initialize LVGL library");
    lv_init();

    // buffer allocation
    lv_color_t *buf1 = heap_caps_malloc(HOR_RES * 20 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1);
    lv_color_t *buf2 = heap_caps_malloc(HOR_RES * 20 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf2);
    // initialize LVGL draw buffers and ddriver
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, HOR_RES * 20);

    // display characteristics
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = HOR_RES;
    disp_drv.ver_res = VER_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
  
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };

    // time handler so that dispy knows passed time for operations or something
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));
}

void keep(){
    lv_obj_t *label6 = lv_label_create(lv_scr_act());
    lv_label_set_text(label6, "soc_status: 0");
    lv_obj_align(label6, LV_ALIGN_BOTTOM_RIGHT, -5, 0);
    struct sockaddr_in ap_info = {0};
    ap_info.sin_family = AF_INET;
    ap_info.sin_port = htons(PORT);
    inet_pton(AF_INET, AP_IP, &ap_info.sin_addr);
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    int w = 0;
    while (1){
        w = setsockopt(soc, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(soc, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(soc, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(soc, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));   
        ESP_LOGI("socket state", "%i", w);     
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        if(w != 0){
            close(soc);
            lv_label_set_text(label6, "soc_status: -1");
            socket_status = -1;
            soc = socket(AF_INET, SOCK_STREAM, 0);
            if(soc < 0){
                ESP_LOGI(TAG_TCP, "Socket creation Failed");
            }
            if(connect(soc, (struct sockaddr *)&ap_info, sizeof(ap_info)) != 0){
                ESP_LOGI(TAG_TCP, "Unable to to connect to %s", inet_ntoa(ap_info.sin_addr.s_addr));
                close(soc);
            }
            else{
                socket_status = 0;
                lv_label_set_text(label6, "soc_status: 0");
            }
            
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    int wifistatus = WIFI_FAILURE;
    // Initialize Non-volatile memory
    esp_err_t storage = nvs_flash_init();
    if(storage == ESP_ERR_NVS_NO_FREE_PAGES || storage == ESP_ERR_NVS_NEW_VERSION_FOUND){
        ESP_ERROR_CHECK(nvs_flash_erase());
        storage = nvs_flash_init();
    }
    ESP_ERROR_CHECK(storage);
    gpio_num_t keypad[8] = {R1, R2, R3, R4, C1, C2, C3, C4};
    // Initialize keyboard
    keypad_initalize(keypad);
    display_initialize();
    // Create display interface 
    lv_obj_t *label1 = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(label1, LV_LABEL_LONG_WRAP); 
    lv_label_set_text_static(label1, "Text received from paired device:");
    lv_obj_align(label1, LV_ALIGN_TOP_MID, 0, 10);
 
    lv_obj_t *obj1 = lv_obj_create(lv_scr_act());
    lv_obj_set_size(obj1, 280, 60);
    lv_obj_align(obj1, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *label2 = lv_label_create(obj1);
    lv_label_set_long_mode(label2, LV_LABEL_LONG_WRAP); 
    lv_label_set_text(label2, "Waiting for message");
    lv_obj_set_width(label2, 240);
    lv_obj_align(label2, LV_ALIGN_CENTER, 0, 0);
    
    lv_obj_t *obj2 = lv_obj_create(lv_scr_act());
    lv_obj_set_size(obj2, 70, 45);
    lv_obj_align(obj2, LV_ALIGN_LEFT_MID, 0, 5);

    label3 = lv_label_create(obj2);
    lv_label_set_long_mode(label3, LV_LABEL_LONG_WRAP); 
    lv_label_set_text(label3, "INerr");
    lv_obj_set_width(label3, 40);
    lv_obj_align(label3, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *obj3 = lv_obj_create(lv_scr_act());
    lv_obj_set_size(obj3, 70, 45);
    lv_obj_align(obj3, LV_ALIGN_RIGHT_MID, 0, 5);

    label4 = lv_label_create(obj3);
    lv_label_set_long_mode(label4, LV_LABEL_LONG_WRAP); 
    lv_label_set_text(label4, "Oerr");
    lv_obj_set_width(label4, 40);
    lv_obj_align(label4, LV_ALIGN_CENTER, 0, 0);
    
    lv_obj_t *label5 = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(label5, LV_LABEL_LONG_WRAP); 
    lv_label_set_text_static(label5, "Text input that will be \nsent to paired device:");
    lv_obj_align(label5, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *txt_area = lv_textarea_create(lv_scr_act());
    lv_obj_set_size(txt_area, 280, 60);
    lv_obj_align(txt_area, LV_ALIGN_CENTER, 0, 65);
   
    // Connect to AP
    wifistatus = connect_wifi();
    if(wifistatus != WIFI_SUCCESS){
        ESP_LOGE(TAG_WI, "Failed to connect to AP");
    }

    // Connect to socket
    wifistatus = socket_connection();
    if(wifistatus != TCP_SUCCESS){
        ESP_LOGE(TAG_TCP, "Failed socket connection");
    }     
    xTaskCreate(socket_read, "Socket receive task", 1024*2, label2, configMAX_PRIORITIES, NULL);
    xTaskCreate(keypadtask, "keypad task", 1024*4, txt_area, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(disRefresh, "disp refresh task", 1024*8, NULL,configMAX_PRIORITIES,NULL);
    xTaskCreate(keep, "alive_task", 1024*2, NULL, configMAX_PRIORITIES-2, NULL);
    }


