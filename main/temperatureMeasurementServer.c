#include <stdio.h>
#include <math.h>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#define LED_PIN 2

#define ADC_CHAN ADC1_CHANNEL_5
#define ADC_ATTEN ADC_ATTEN_DB_11

#define T0 25
#define B 3950
#define TR 10000
#define RR 10000
#define MAX_V 3300
//Measurement delay
#define M_DELAY 10000
#define METERING_NUM 100

#define WIFI_SSID			CONFIG_WIFI_SSID
#define WIFI_PASSWORD		CONFIG_WIFI_PASSWORD
#define MAX_RETRY			CONFIG_MAX_RETRY
#define STATIC_IP_ADDR		CONFIG_STATIC_IP_ADDR
#define STATIC_NETMASK_ADDR CONFIG_STATIC_NETMASK_ADDR
#define STATIC_GW_ADDR		CONFIG_STATIC_GW_ADDR
#define WIFI_CONNECTED		BIT0
#define WIFI_FAIL			BIT1

#define SERVER_PORT			CONFIG_TCP_SERVER_PORT

const char *TAG_WIFI = "wifi";
const char *TAG_SERVER = "tcp server";

esp_adc_cal_characteristics_t adc1_chars;
float temperature = 0.;

EventGroupHandle_t wifi_event_group;
int retry_num;

void blink(void* param){
	gpio_reset_pin(LED_PIN);
	gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

	while(1){
		gpio_set_level(LED_PIN, 1);
		vTaskDelay(pdMS_TO_TICKS(500));
		gpio_set_level(LED_PIN, 0);
		vTaskDelay(pdMS_TO_TICKS(500));
	}
}

TaskHandle_t blink_init(void){
	TaskHandle_t blink_handle;
	xTaskCreate(
		blink,			//Pointer to the task entry function.
		"blink",		//Task name.
		2048,			//The size of the task stack specified as the number of bytes.
		NULL,			//Pointer that will be used as the parameter for the task being created.
		1,				//The priority at which the task should run.
		&blink_handle	// Used to pass back a handle by which the created task can be referenced.
	);	
	configASSERT(blink_handle);
	return blink_handle;
}

bool adc_calibration_init(void){
	const char* TAG = "ADC CALI";
	bool cali_enable = false;

	esp_err_t ret = esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_VREF);
	if (ret == ESP_OK){
		cali_enable = true;
		esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN, ADC_WIDTH_BIT_DEFAULT, 0, &adc1_chars);
	} else{
		ESP_LOGE(TAG, "Calibration scheme not supported");
	}
	return cali_enable;
}

void temperature_metering(void* param){
	int adc_raw = 0;
	int voltage = 0;
	int resistance = 0;
	bool cali_enable = adc_calibration_init();
	if(!cali_enable){
		return;
	}

	ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_DEFAULT));
	ESP_ERROR_CHECK(adc1_config_channel_atten(ADC_CHAN, ADC_ATTEN));

	while(1) {
		adc_raw = 0;
		for (int i = 0; i < METERING_NUM; i++){
			adc_raw += adc1_get_raw(ADC_CHAN);
			vTaskDelay(pdMS_TO_TICKS(1));
		}
		adc_raw /= METERING_NUM;
		voltage = esp_adc_cal_raw_to_voltage(adc_raw, &adc1_chars);
		resistance = ((MAX_V - voltage)*RR)/voltage;
		temperature = 1./(1./(T0+273.15)+log((float)resistance/TR)/B) - 273.15;

		vTaskDelay(pdMS_TO_TICKS(M_DELAY-METERING_NUM));
	}
}

TaskHandle_t temperature_metering_init(void){
	TaskHandle_t temp_handle;
	xTaskCreate(
		temperature_metering,	//Pointer to the task entry function.
		"temp metering",		//Task name.
		2048,					//The size of the task stack specified as the number of bytes.
		NULL,					//Pointer that will be used as the parameter for the task.
		1,						//The priority at which the task should run.
		&temp_handle			// Used to pass back a handle by which the task can be referenced.
	);	
	configASSERT(temp_handle);
	return temp_handle;
}

void set_static_ip(esp_netif_t *netif){
	if(esp_netif_dhcpc_stop(netif) != ESP_OK){
		ESP_LOGE(TAG_WIFI, "failed to stop dhcp client");
		return;
	}
	esp_netif_ip_info_t ip;
	memset(&ip, 0, sizeof(esp_netif_ip_info_t));
	ip.ip.addr = ipaddr_addr(STATIC_IP_ADDR);
	ip.netmask.addr = ipaddr_addr(STATIC_NETMASK_ADDR);
	ip.gw.addr = ipaddr_addr(STATIC_GW_ADDR);
	if (esp_netif_set_ip_info(netif, &ip) != ESP_OK) {
		ESP_LOGE(TAG_WIFI, "Failed to set ip info");
		return;
	}
	ESP_LOGD(TAG_WIFI, "success to set static ip: %s;", STATIC_IP_ADDR);
}

void event_handler(void* arg, esp_event_base_t event_base, int event_id, void* event_data){
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START){
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED){
		set_static_ip(arg);
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED){
		ESP_LOGI(TAG_WIFI, "connect to the AP fail");
		if (retry_num < MAX_RETRY){
			ESP_LOGI(TAG_WIFI, "retry to connect to the AP");
			esp_wifi_connect();
			retry_num++;
		} else {
			xEventGroupSetBits(wifi_event_group, WIFI_FAIL);
		}
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP){
		ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
		ESP_LOGI(TAG_WIFI, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		retry_num = 0;
		xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED);
	}
}

int wifi_init(void){
	int status = WIFI_FAIL;
	wifi_event_group = xEventGroupCreate();

	//Initialize the underlying TCP/IP stack.
	ESP_ERROR_CHECK(esp_netif_init());
	
	//Loop used for system events (WiFi events, for example).
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
	assert(sta_netif);

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;
	ESP_ERROR_CHECK(
		esp_event_handler_instance_register(
			WIFI_EVENT,
			ESP_EVENT_ANY_ID,
			&event_handler,
			sta_netif,
			&instance_any_id
		)
	);
	ESP_ERROR_CHECK(
		esp_event_handler_instance_register(
			IP_EVENT,
			IP_EVENT_STA_GOT_IP,
			&event_handler,
			sta_netif,
			&instance_got_ip
		)
	);
	wifi_config_t wifi_config = {
		.sta = {
			.ssid = WIFI_SSID,
			.password = WIFI_PASSWORD,
			.threshold.authmode = WIFI_AUTH_WPA2_PSK,
		},
	};

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_LOGI(TAG_WIFI, "wifi_init finish");

	EventBits_t bits = xEventGroupWaitBits(
		wifi_event_group,
		WIFI_CONNECTED | WIFI_FAIL,
		pdFALSE,
		pdFALSE,
		portMAX_DELAY);
												
	if (bits & WIFI_CONNECTED){
		ESP_LOGI(TAG_WIFI, "connected to ap SSID: %s password: %s", WIFI_SSID, WIFI_PASSWORD);
		status = WIFI_CONNECTED;
	} else if (bits & WIFI_FAIL){
		ESP_LOGI(TAG_WIFI, "fail to connect to ap SSID: %s password: %s", WIFI_SSID, WIFI_PASSWORD);
		status = WIFI_FAIL;
	} else {
		ESP_LOGE(TAG_WIFI, "UNEXPECTED EVENT");
		status = WIFI_FAIL;
	}
	return status;
}

void tcp_server_connection(const int cfd){
	int recv_buf_size = 256;
	char recv_buf[recv_buf_size]; 
	int send_buf_size = 256;
	char send_buf[send_buf_size]; 
	recv(cfd, recv_buf, recv_buf_size, 0);
	ESP_LOGI("conn", "recv data [%s] from %d;", recv_buf, cfd);
	if (!strcmp(recv_buf, "TEMPERATURE")){
		send_buf_size = snprintf(send_buf, send_buf_size, "%.2f", temperature);
	} else {
		send_buf_size = snprintf(send_buf, send_buf_size, "can't process recv msg");
	}
	send(cfd, send_buf, send_buf_size, 0);
	ESP_LOGI("conn", "send data [%s] to %d;", send_buf, cfd);	
	ESP_LOGI("conn", "close connection: %d;", cfd);
}

void tcp_server(void* param){
	int sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd < 0){
		ESP_LOGE(TAG_SERVER, "unable to create socket: %s;", strerror(errno));
		vTaskDelete(NULL);
		return;
	}
	ESP_LOGI(TAG_SERVER, "socket created");
	
	struct sockaddr_in server_info;
	server_info.sin_family = AF_INET;
	server_info.sin_port = htons(SERVER_PORT);
	server_info.sin_addr.s_addr = INADDR_ANY;
	int err = bind(sfd, (struct sockaddr*) &server_info, sizeof(server_info));
	if (err != 0){
		ESP_LOGE(TAG_SERVER, "bind error: %s;", strerror(errno));
		close(sfd);
		vTaskDelete(NULL);
		return;
	}
	ESP_LOGI(TAG_SERVER, "socket bound, port %d", SERVER_PORT);

	err = listen(sfd, 3);
	if (err != 0){
		ESP_LOGE(TAG_SERVER, "while listen error: %s;", strerror(errno));
		close(sfd);
		vTaskDelete(NULL);
		return;
	}

	while (1){
		ESP_LOGI(TAG_SERVER, "listening...");

		struct sockaddr client_info;
		socklen_t client_info_len = sizeof(client_info);
		int cfd = accept(sfd, &client_info, &client_info_len);
		if (cfd < 0) {
			ESP_LOGE(TAG_SERVER, "unable to accept connection: %s", strerror(errno));
			break;
		}

		char client_addr_str[64];
		inet_ntoa_r(
			((struct sockaddr_in*)&client_info)->sin_addr,
			client_addr_str,
			sizeof(client_addr_str)-1
		);

		ESP_LOGI(TAG_SERVER, "socket accepted ip address: %s;", client_addr_str);

		tcp_server_connection(cfd);

		shutdown(cfd, 0);
		close(cfd);
	}
	close(sfd);
	vTaskDelete(NULL);
}

void tcp_server_init(){
	xTaskCreate(
		tcp_server,	 //Pointer to the task entry function.
		"tcp_server",//Task name.
		4096,        //The size of the task stack specified as the number of bytes.
		NULL,        //Pointer that will be used as the parameter for the task being created.
		2,           //The priority at which the task should run.
		NULL        // Used to pass back a handle by which the created task can be referenced.
	);		
}

void app_main(void){
	blink_init();
	temperature_metering_init();

	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND){
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	int status = wifi_init();
	if (status != WIFI_CONNECTED){
		vTaskDelete(NULL);
		return;
	}
	tcp_server_init();
}
