#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"
#include "protocol_examples_common.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include <ultrasonic.h>
#include <dht.h>

#ifdef CONFIG_EXAMPLE_IPV4
#define HOST_IP_ADDR CONFIG_EXAMPLE_IPV4_ADDR
#else
#define HOST_IP_ADDR CONFIG_EXAMPLE_IPV6_ADDR
#endif

#define PORT CONFIG_EXAMPLE_PORT
#define DHT_GPIO                    GPIO_NUM_14
#define TRIG                        GPIO_NUM_0
#define ECHO                        GPIO_NUM_2
#define MAX_DISTANCE_CM             500

static const dht_sensor_type_t sensor_type = DHT_TYPE_DHT11;
static const gpio_num_t dht_gpio = DHT_GPIO;
static const char *TAG = "->";

QueueHandle_t bufferTemperatura; 
QueueHandle_t bufferUmidade;
QueueHandle_t bufferDistancia;

static void task_tcpserver(void *pvParameters);
static void do_retransmit(const int sock);
void task_dht(void *pvParameters);
void task_ultrasonic(void *pvParamters);

void task_dht(void *pvParamters){
    int16_t temperatura;
    int16_t umidade;

    gpio_set_pull_mode(dht_gpio, GPIO_PULLUP_ONLY);
    
    while(1){
        if(dht_read_data(sensor_type, dht_gpio, &umidade, &temperatura) == ESP_OK){
            umidade     = umidade     / 10;
            temperatura = temperatura / 10;

            //ESP_LOGE(TAG, "Umidade: %d", umidade);
            //ESP_LOGE(TAG, "Temperatura: %d", temperatura);
        
            xQueueSend(bufferTemperatura, &temperatura, pdMS_TO_TICKS(0));
            xQueueSend(bufferUmidade, &umidade, pdMS_TO_TICKS(0));
        } else {
            ESP_LOGE(TAG, "Não foi possível se conectar com o sensor");
        }
       
        vTaskDelay(1500/portTICK_PERIOD_MS);
    }
}

void task_ultrasonic(void *pvParamters) {
    ultrasonic_sensor_t sensor = {
		.trigger_pin = TRIG,
		.echo_pin = ECHO};
	ultrasonic_init(&sensor);

	while (1) {
        uint32_t distancia = 0;
        esp_err_t res = ultrasonic_measure_cm(&sensor, MAX_DISTANCE_CM, &distancia);
        
        if (res == ESP_OK) {        
            //ESP_LOGE(TAG, "Distancia: %d cm", distancia);
            xQueueSend(bufferDistancia,&distancia,pdMS_TO_TICKS(0));
        } else {
            ESP_LOGE(TAG, "Problema com sensor de distancia.");
        }

        vTaskDelay(500 / portTICK_PERIOD_MS);
	}
}

static void task_tcpserver(void *pvParameters) {
    char addr_str[128];
    int addr_family;
    int ip_protocol;


#ifdef CONFIG_EXAMPLE_IPV4
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);
    addr_family = AF_INET;
    ip_protocol = IPPROTO_IP;
    inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
#else
    struct sockaddr_in6 dest_addr;
    bzero(&dest_addr.sin6_addr.un, sizeof(dest_addr.sin6_addr.un));
    dest_addr.sin6_family = AF_INET6;
    dest_addr.sin6_port = htons(PORT);
    addr_family = AF_INET6;
    ip_protocol = IPPROTO_IPV6;
    inet6_ntoa_r(dest_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
#endif

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    while (1) {
        ESP_LOGI(TAG, "Socket listening");

        struct sockaddr_in6 source_addr;
        uint addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        if (source_addr.sin6_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
        } else if (source_addr.sin6_family == PF_INET6) {
            inet6_ntoa_r(source_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
        }
        ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

        do_retransmit(sock);

        shutdown(sock, 0);
        close(sock);
    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}

static void do_retransmit(const int sock) {
    uint16_t temperatura;
    uint16_t umidade;
    uint16_t distancia;
    int len;
    char rx_buffer[128];
	char stringTemperatura[100];
    char stringUmidade[100];
    char stringDistancia[100];

    do {
        len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        if (len < 0) {
            ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
        } else if (len == 0) {
            ESP_LOGW(TAG, "Connection closed");
        } else {
			//Alguma mensagem foi recebida.
            rx_buffer[len] = 0;
            ESP_LOGI(TAG, "Received %d bytes: %s", len, rx_buffer);

			if(len>=4 && rx_buffer[0]=='T' && rx_buffer[1]=='E' && rx_buffer[2]=='M' && rx_buffer[3]=='P') {
				xQueueReceive(bufferTemperatura, &temperatura, pdMS_TO_TICKS(2000));
				sprintf(stringTemperatura, "Temperatura: %d C ", temperatura);
				send(sock, stringTemperatura, strlen(stringTemperatura), 0);
			}

			if(len>=4 && rx_buffer[0]=='U' && rx_buffer[1]=='M' && rx_buffer[2]=='I' && rx_buffer[3]=='D') {
				xQueueReceive(bufferUmidade, &umidade, pdMS_TO_TICKS(2000));
				sprintf(stringUmidade, "Umidade: %d ", umidade);
				send(sock, stringUmidade, strlen(stringUmidade), 0);
			}

			if(len>=4 && rx_buffer[0]=='D' && rx_buffer[1]=='I' && rx_buffer[2]=='S' && rx_buffer[3]=='T') {
				xQueueReceive(bufferDistancia, &distancia, pdMS_TO_TICKS(2000));
				sprintf(stringDistancia, "Distancia: %d cm ", distancia);
				send(sock, stringDistancia, strlen(stringDistancia), 0);
			}                      
        }
    } while (len > 0);
}

void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    bufferTemperatura   = xQueueCreate(5,sizeof(uint16_t)); 
    bufferUmidade       = xQueueCreate(5,sizeof(uint16_t));
    bufferDistancia     = xQueueCreate(5,sizeof(uint16_t));

    xTaskCreate(task_ultrasonic, "task_ultrasonic", configMINIMAL_STACK_SIZE * 2, NULL, 3, NULL);
    xTaskCreate(task_dht, "task_dht", configMINIMAL_STACK_SIZE * 2, NULL, 3, NULL);
    xTaskCreate(task_tcpserver, "task_tcpserver", configMINIMAL_STACK_SIZE * 5, NULL, 2, NULL);
}
