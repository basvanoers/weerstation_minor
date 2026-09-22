#include <stdio.h>


#include <driver/gpio.h>
#include "dht.h"
#include <esp_adc/adc_oneshot.h>

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_random.h"

#include "lib_ssd1680.h"
#include "ssd1680_fonts.h"
#include "eye_122_250.h"
#include "c64_122_250.h"
#include "test_122_250.h"
#include "test_ram_122_250.h"
#include "image_sun_1_70_66.h"
#include "image_cloud_2_70_59.h"
#include "esp_wifi.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include <bmp280.h>
// Set your GPIO pin and Sensor Type directly
#define DHT_GPIO_PIN GPIO_NUM_7
#define SENSOR_TYPE  DHT_TYPE_AM2301 // AM2301 is DHT22 / AM2302
#define ADC_PIN       ADC_CHANNEL_5     // Channel 7 - Check ESP32 Pinout for the GPIO Number
#define ADC_UNIT      ADC_UNIT_1        // ADC1
#define ADC_BITWIDTH  ADC_BITWIDTH_12   // 12-bit resolution (0-4095)
#define ADC_ATTEN     ADC_ATTEN_DB_12   // ~3.3V full-scale voltage
static const char *TAG = "main";
#define wifi_SSID "iotroam"
#define wifi_password  "ihS@e6IBvNmiSQeJ"

#define EPAPER_HOST    SPI2_HOST
#define EPAPER_RES_X	128
#define EPAPER_RES_Y	296

#define PIN_NUM_MISO 9	// D6
#define PIN_NUM_MOSI 8	// D5
#define PIN_NUM_CLK  4	// D4
#define PIN_NUM_CS   1	// D1

#define PIN_NUM_DC   2	// D3
#define PIN_NUM_RST  3	// DO
#define PIN_NUM_BCKL 5  // D2


#define CONFIG_EXAMPLE_I2C_MASTER_SDA 12
#define CONFIG_EXAMPLE_I2C_MASTER_SCL 13
#define EXAMPLE_ESP_WIFI_SSID      "iotroam"
#define EXAMPLE_ESP_WIFI_PASS      "ihS@e6IBvNmiSQeJ"
#define EXAMPLE_ESP_MAXIMUM_RETRY   10



typedef struct{
    float Temperatuur;
    float Humidity;
    float Lux;
}Data;


/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1



static int s_retry_num = 0;


static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

   wifi_config_t wifi_config = {
    .sta = {
        .ssid = EXAMPLE_ESP_WIFI_SSID,
        .password = EXAMPLE_ESP_WIFI_PASS,
        .threshold.authmode = WIFI_AUTH_WPA2_PSK,
    },
};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}




void static font_orientation_demo(ssd1680_t *disp, ssd1680_color_t color)
{
	ssd1680_font_t * font = &font_terminal_9pt;

	ssd1680_change_orientation(disp, SSD1680_270_DEG);
	ssd1680_display_string(disp, font, 180, 16, "Hallo", color);
    ssd1680_display_string(disp, font, 2, 40, "Banaan", color);
	ssd1680_send_framebuffer(disp);
	ssd1680_refresh(disp, FAST_FULL_REFRESH);

}
static void Update(Data update_data,
                   ssd1680_t *disp,
                   ssd1680_color_t color)
{
    ssd1680_font_t *font = &font_terminal_9pt;

    ssd1680_change_orientation(disp, SSD1680_90_DEG);

    ssd1680_display_string(
        disp,
        font,
        30,
        16,
        "Test...",
        color
    );

    ssd1680_send_framebuffer(disp);
    ssd1680_refresh(disp, FAST_FULL_REFRESH);
}
static void ADC(void *pvParameters)
{
    
    int adc_value;
    adc_oneshot_unit_handle_t adc_handle;

    // Initialize ADC Oneshot Mode Driver on the ADC Unit
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    // Configure ADC channel
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH,
        .atten = ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_PIN, &config));
    while(1)
    {
        // Read ADC value with Oneshot
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_PIN, &adc_value));
        // Print ADC value
        
       float volts = ((float)adc_value / 4095.0f) * 3.3f;
       
       float uAmps = (volts / 10000) * 1e6;
       float lux = uAmps / 0.5;
       ESP_LOGI("lux", "%f", lux);
       vTaskDelay(1000 / portTICK_PERIOD_MS); 
    }

}
  
void DHT(void *pvParameters)
{
    Data temp_meet_data;
    float temperature = 0.0f;
    float humidity = 0.0f;
    gpio_set_pull_mode(DHT_GPIO_PIN, GPIO_PULLUP_ONLY);
    while (1)
    {
        
        esp_err_t res = dht_read_float_data(SENSOR_TYPE, DHT_GPIO_PIN, &humidity, &temperature);
        vTaskDelay(pdMS_TO_TICKS(100));
        if (res == ESP_OK) {
            ESP_LOGI(TAG, "Humidity: %.1f%% | Temperature: %.1f°C", humidity, temperature);
            temp_meet_data.Temperatuur = temperature;
            temp_meet_data.Humidity = humidity;
            
        } else {
            ESP_LOGE(TAG, "Could not read data from sensor (Error code: 0x%x)", res);
        }
        //*pvParameters = temp_meet_data;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
void E_ink(void *pvParameters)
{
    
   ssd1680_t * ssd1680_disp;
	spi_host_device_t spi_host = EPAPER_HOST;

	ssd1680_pinmap_t ssd1680_pinmap = {
		    .busy = PIN_NUM_BCKL,
		    .reset = PIN_NUM_RST,
		    .dc = PIN_NUM_DC,
		    .cs = PIN_NUM_CS
	};

	//Initialize NVSPIN_NUM_BCKL
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    //esp_err_t ret;
    //spi_device_handle_t spi;
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8192,
		.flags = SPICOMMON_BUSFLAG_MASTER,
    };

    //Initialize the SPI bus
    ret = spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);

    uint8_t ssd1680_orientation = SSD1680_NORMAL;
    //uint8_t ssd1680_orientation = SSD1680_90_DEG;
    //uint8_t ssd1680_orientation = SSD1680_180_DEG;
    //uint8_t ssd1680_orientation = SSD1680_270_DEG;
    ssd1680_disp = ssd1680_init(spi_host, ssd1680_pinmap, EPAPER_RES_X, EPAPER_RES_Y, ssd1680_orientation);
    Data data;
    while(1)
    {
      Update(data,ssd1680_disp, SSD1680_BLACK);
      vTaskDelay(pdMS_TO_TICKS(1000));

    }

}

void WIFI(void *pvParameters){
    ESP_LOGI(TAG, "Starting DHT22 Application...");
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
        /* If you only want to open more logs in the wifi module, you need to make the max level greater than the default level,
         * and call esp_log_level_set() before esp_wifi_init() to improve the log level of the wifi module. */
        esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
    }

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
}
void BME(void *pvParameters)
{
 bmp280_params_t params;
    bmp280_init_default_params(&params);
    bmp280_t dev;
    memset(&dev, 0, sizeof(bmp280_t));


    ESP_ERROR_CHECK(bmp280_init_desc(&dev, BMP280_I2C_ADDRESS_0, 0, CONFIG_EXAMPLE_I2C_MASTER_SDA, CONFIG_EXAMPLE_I2C_MASTER_SCL));
    printf("Initializing BME280...\n");





esp_err_t ret = bmp280_init(&dev, &params);
 printf("Initializing BME280...\n");

if (ret != ESP_OK) {
    printf("BME280 init failed: %s\n", esp_err_to_name(ret));
    vTaskDelete(NULL);
    return;
}

    bool bme280p = dev.id == BME280_CHIP_ID;
    printf("BMP280: found %s\n", bme280p ? "BME280" : "BMP280");

    float pressure, temperature, humidity;

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (bmp280_read_float(&dev, &temperature, &pressure, &humidity) != ESP_OK)
        {
            printf("Temperature/pressure reading failed\n");
            continue;
        }

        /* float is used in printf(). you need non-default configuration in
         * sdkconfig for ESP8266, which is enabled by default for this
         * example. see sdkconfig.defaults.esp8266
         */
        printf("Pressure: %.2f Pa, Temperature: %.2f C", pressure, temperature);
        if (bme280p)
            printf(", Humidity: %.2f\n", humidity);
        else
            printf("\n");
    }   
}
void app_main(void)
{
    Data *Meetdata;
    

    ESP_ERROR_CHECK(i2cdev_init());
    //xTaskCreate(BME, "BME", 4096,NULL,3,NULL);
    vTaskDelay(pdMS_TO_TICKS(300));
    xTaskCreate(DHT, "dht_test", 4096,NULL, 10, NULL);
    vTaskDelay(pdMS_TO_TICKS(300));
  xTaskCreate(ADC, "daf",4096,NULL,3,NULL);
    vTaskDelay(pdMS_TO_TICKS(300));
    xTaskCreate(E_ink, "E_ink",4096,NULL,3,NULL);
    vTaskDelay(pdMS_TO_TICKS(300));
    //xTaskCreate(WIFI,"wifi", 4096,NULL,3,NULL);
 

   
    
    
    
    
    
    
    
    
    
}