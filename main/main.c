#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "dht.h"
#include <esp_adc/adc_oneshot.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"

#include "lib_ssd1680.h"
#include "ssd1680_fonts.h"
#include "image_sun_1_70_66.h"
#include "image_thermometer_1_70_66.h"
#include "image_logo_1_100_60.h"
#include "image_sun_1_100_60.h"
#include "image_sun_1_100_100.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "lwip/sockets.h"

#define DHT_GPIO_PIN       GPIO_NUM_7
#define SENSOR_TYPE        DHT_TYPE_AM2301

#define ADC_PIN            ADC_CHANNEL_5
#define ADC_UNIT           ADC_UNIT_1
#define ADC_BITWIDTH       ADC_BITWIDTH_12
#define ADC_ATTEN          ADC_ATTEN_DB_12

#define BUTTON_GPIO GPIO_NUM_13

static volatile uint64_t last_isr_time = 0;
static volatile uint32_t counter = 0;
static QueueHandle_t button_queue;


//#define WIFI_SSID "iotroam"
 //#define WIFI_PASS  "ihS@e6IBvNmiSQeJ"


#define WIFI_SSID "KPNA2FD36_ExtraWiFi2"
#define WIFI_PASS "FW1zFwmkKWzjj3T4"

#define TIME_ZONE   "Europe/Amsterdam"

#define API_URL     "https://timeapi.io/api/time/current/zone?timeZone=" TIME_ZONE

#define EPAPER_HOST        SPI2_HOST

#define EPAPER_RES_X       128
#define EPAPER_RES_Y       296

#define PIN_NUM_MISO       9
#define PIN_NUM_MOSI       8
#define PIN_NUM_CLK        4
#define PIN_NUM_CS         1

#define PIN_NUM_DC         2
#define PIN_NUM_RST        3

#define PIN_NUM_BCKL       5

#define STATION_ID         "bas"
#define API_KEY            "61f1503117b194339a807ff89dd7cd7d30d79ca4928e1fca69921ba2ff2c0e5e"
#define MQTT_HOST          "145.24.237.237"
#define MQTT_PORT          8883

#define MQTT_INTERVAL_MS   15000      
#define MQTT_KEEPALIVE_S   60         
#define MQTT_RECONNECT_US  (5 * 1000000LL)


#define WIND_MS_PER_RPM    1.0f


static const char *TAG = "http_get";
static const char *MQTT_TAG = "homestation";

#define BUF_SIZE    512
static char response_buf[BUF_SIZE];
static int  response_len = 0;
volatile uint32_t pulse_count = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (response_len + copy >= BUF_SIZE) copy = BUF_SIZE - response_len - 1;
        memcpy(response_buf + response_len, evt->data, copy);
        response_len += copy;
        response_buf[response_len] = '\0';
    }
    return ESP_OK;
}

typedef struct
{
    float Temperatuur;
    float Humidity;
    float Lux;
    char  Time[8];
    char Place[32];
    float Wind_speed;
    bool  Dht_valid;   // true zodra de DHT minstens eenmaal gelezen is

} Data;



static Data shared_data =
{
    .Temperatuur = 0.0f,
    .Humidity = 0.0f,
    .Lux = 0.0f,
    .Time = "--:--",
    .Place = "...",
    .Wind_speed = 0.0f,
    .Dht_valid = false
};
static SemaphoreHandle_t data_mutex;

static int get_json_value(const char *json, const char *key, char *out, int out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    int is_string = (*p == '"');
    if (is_string) p++;
    int i = 0;
    while (*p && i < out_size - 1) {
        if (is_string && *p == '"') break;
        if (!is_string && (*p == ',' || *p == '}')) break;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i;
}
static bool http_get(const char *url, bool https)
{
    response_len = 0;
    memset(response_buf, 0, BUF_SIZE);

    esp_http_client_config_t config = {
        .url           = url,
        .event_handler = http_event_handler,
        .timeout_ms    = 8000,
    };
    if (https) config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    bool ok = (err == ESP_OK && esp_http_client_get_status_code(client) == 200);
    if (!ok) ESP_LOGW(TAG, "GET failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return ok;
}

static void fetch_time(void)
{
    if (!http_get(API_URL, true)) return;

    char t[20];
    if (get_json_value(response_buf, "time", t, sizeof(t)) &&
        xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        strlcpy(shared_data.Time, t, sizeof(shared_data.Time));
        xSemaphoreGive(data_mutex);
    }
}

static bool fetch_place(void)
{

   if (!http_get("http://ipwho.is/?fields=city", false)) return false;

    char city[32];
    if (!get_json_value(response_buf, "city", city, sizeof(city))) return false;

    ESP_LOGI(TAG, "Place: %s", city);

    if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strlcpy(shared_data.Place, city, sizeof(shared_data.Place));
        xSemaphoreGive(data_mutex);
        return true;
    }
    return false;
}

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0


static void wifi_event_handler(void *arg, esp_event_base_t base,
int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA started. Connecting to %s...", WIFI_SSID);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason: %d", d->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP Address: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


static void wifi_init(void)
{

    s_wifi_event_group = xEventGroupCreate();


    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());


    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));


    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);


    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
     
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(40));   

    ESP_LOGI(TAG, "Connecting to Wi-Fi...");


    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

static int     s_mqtt_sock = -1;
static char    s_mqtt_client_id[24];
static int64_t s_mqtt_last_attempt_us = 0;
static bool    s_mqtt_attempted = false;

static void mqtt_close(void)
{
    if (s_mqtt_sock >= 0) {
        close(s_mqtt_sock);
        s_mqtt_sock = -1;
    }
}

static bool mqtt_send_all(const uint8_t *buf, size_t len)
{
    while (len > 0) {
        int n = send(s_mqtt_sock, buf, len, 0);
        if (n <= 0) return false;
        buf += n;
        len -= n;
    }
    return true;
}


static size_t mqtt_encode_remaining_length(uint8_t *out, size_t len)
{
    size_t i = 0;
    do {
        uint8_t b = len % 128;
        len /= 128;
        if (len > 0) b |= 0x80;
        out[i++] = b;
    } while (len > 0);
    return i;
}


static bool mqtt_socket_alive(void)
{
    uint8_t tmp[16];
    int n = recv(s_mqtt_sock, tmp, sizeof(tmp), MSG_DONTWAIT);
    if (n == 0) return false;                                  
    if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) return false;
    return true;
}

static bool mqtt_connect(void)
{
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MQTT_PORT);
    if (inet_pton(AF_INET, MQTT_HOST, &addr.sin_addr) != 1) {
        ESP_LOGE(MQTT_TAG, "Ongeldig broker-adres");
        return false;
    }

    s_mqtt_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_mqtt_sock < 0) {
        ESP_LOGE(MQTT_TAG, "Socket maken mislukt: errno %d", errno);
        return false;
    }

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(s_mqtt_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s_mqtt_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(s_mqtt_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(MQTT_TAG, "TCP verbinden mislukt: errno %d", errno);
        mqtt_close();
        return false;
    }

  
    size_t id_len = strlen(s_mqtt_client_id);
    uint8_t pkt[64];
    size_t pos = 0;
    pkt[pos++] = 0x10;                                         
    pos += mqtt_encode_remaining_length(&pkt[pos], 10 + 2 + id_len);
    static const uint8_t vh[] = { 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02 };
    memcpy(&pkt[pos], vh, sizeof(vh));                          
    pos += sizeof(vh);
    pkt[pos++] = MQTT_KEEPALIVE_S >> 8;
    pkt[pos++] = MQTT_KEEPALIVE_S & 0xFF;
    pkt[pos++] = id_len >> 8;
    pkt[pos++] = id_len & 0xFF;
    memcpy(&pkt[pos], s_mqtt_client_id, id_len);
    pos += id_len;

    if (!mqtt_send_all(pkt, pos)) {
        ESP_LOGE(MQTT_TAG, "CONNECT versturen mislukt");
        mqtt_close();
        return false;
    }

    
    uint8_t ack[4];
    size_t got = 0;
    while (got < sizeof(ack)) {
        int n = recv(s_mqtt_sock, ack + got, sizeof(ack) - got, 0);
        if (n <= 0) {
            ESP_LOGE(MQTT_TAG, "Geen CONNACK ontvangen");
            mqtt_close();
            return false;
        }
        got += n;
    }
    if (ack[0] != 0x20 || ack[1] != 0x02 || ack[3] != 0x00) {
        ESP_LOGE(MQTT_TAG, "Broker weigert verbinding, code %d", ack[3]);
        mqtt_close();
        return false;
    }

    ESP_LOGI(MQTT_TAG, "MQTT verbonden");
    return true;
}

static bool mqtt_ensure_connected(void)
{
    if (s_mqtt_sock >= 0 && mqtt_socket_alive()) return true;
    mqtt_close();

    // Probeer maximaal eenmaal per vijf seconden opnieuw te verbinden.
    int64_t now = esp_timer_get_time();
    if (s_mqtt_attempted && (now - s_mqtt_last_attempt_us) < MQTT_RECONNECT_US) return false;
    s_mqtt_attempted = true;
    s_mqtt_last_attempt_us = now;

    return mqtt_connect();
}


static void homestation_mqtt_start(void)
{
    
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mqtt_client_id, sizeof(s_mqtt_client_id), "hs-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}


static bool homestation_publish_value(const char *metric, float value)
{
    if (!mqtt_ensure_connected()) return false;

    char topic[96];
    char payload[160];
    int tl = snprintf(topic, sizeof(topic), "homestation/%s/%s", STATION_ID, metric);
    int pl = snprintf(payload, sizeof(payload), "{\"apiKey\":\"%s\",\"value\":%.2f}", API_KEY, value);
    if (tl < 0 || tl >= (int)sizeof(topic) || pl < 0 || pl >= (int)sizeof(payload)) return false;

    
    uint8_t pkt[1 + 4 + 2 + sizeof(topic) + sizeof(payload)];
    size_t pos = 0;
    pkt[pos++] = 0x30;
    pos += mqtt_encode_remaining_length(&pkt[pos], 2 + tl + pl);
    pkt[pos++] = tl >> 8;
    pkt[pos++] = tl & 0xFF;
    memcpy(&pkt[pos], topic, tl);   pos += tl;
    memcpy(&pkt[pos], payload, pl); pos += pl;

    if (!mqtt_send_all(pkt, pos)) {
        mqtt_close();               
        return false;
    }
    return true;
}


static void homestation_send_custom_data(const char *slug, float value)
{
    if (!isfinite(value)) return;

    char metric[64];
    snprintf(metric, sizeof(metric), "custom/%s", slug);
    if (!homestation_publish_value(metric, value)) {
        ESP_LOGE(MQTT_TAG, "Eigen meting verzenden mislukt");
    }
}


static void homestation_send_data(float temperature, float humidity, float wind_speed)
{
    if (!isfinite(temperature) || !isfinite(humidity) || !isfinite(wind_speed) ||
        humidity < 0 || humidity > 100 || wind_speed < 0) {
        ESP_LOGE(MQTT_TAG, "Ongeldige sensorwaarden");
        return;
    }
    
    bool t = homestation_publish_value("temperature", temperature);
    bool h = homestation_publish_value("humidity", humidity);
    bool w = homestation_publish_value("windSpeed", wind_speed);

    
    if (t && h && w) ESP_LOGI(MQTT_TAG, "Drie MQTT-berichten verzonden");
    else             ESP_LOGE(MQTT_TAG, "MQTT verzenden mislukt");
}

static void MQTT(void *pvParameters)
{
    Data data;

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(MQTT_INTERVAL_MS));

        if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        data = shared_data;
        xSemaphoreGive(data_mutex);

        
        if (!data.Dht_valid) continue;

        homestation_send_data(data.Temperatuur, data.Humidity, data.Wind_speed * WIND_MS_PER_RPM);
        homestation_send_custom_data("illuminance", data.Lux);
    }
}

// ---------------- Wind sensor (hall) ----------------
#define WIND_GPIO             GPIO_NUM_13
#define WIND_PULSES_PER_REV   1                 // number of magnets on the rotor
#define WIND_MIN_PULSE_US     1              // debounce: ignore pulses closer than 2 ms
#define WIND_WINDOW_MS        1000              // calculation interval
#define WIND_TIMEOUT_US       (5 * 1000000LL)   // no pulse for 5 s -> start fresh

static portMUX_TYPE      wind_mux      = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t wind_pulses   = 0;
static volatile int64_t  wind_first_us = 0;
static volatile int64_t  wind_last_us  = 0;

static void IRAM_ATTR wind_isr(void *arg)
{
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL_ISR(&wind_mux);
    if (now - wind_last_us >= WIND_MIN_PULSE_US) {
        if (wind_pulses == 0) wind_first_us = now;
        wind_last_us = now;
        wind_pulses++;
    }
    portEXIT_CRITICAL_ISR(&wind_mux);
}

static void Wind_speed(void *pvParameters)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << WIND_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     // hall sensors are usually open-drain
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,      // pulse = falling edge
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = already installed
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(WIND_GPIO, wind_isr, NULL));

    int64_t ref_us = 0;   // time of the last pulse of the previous window

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(WIND_WINDOW_MS));

        portENTER_CRITICAL(&wind_mux);
        uint32_t n     = wind_pulses;
        int64_t  first = wind_first_us;
        int64_t  last  = wind_last_us;
        wind_pulses = 0;
        float wind_speed = 0.0f;
        portEXIT_CRITICAL(&wind_mux);

        float rpm = 0.0f;

        if (n > 0) {
            // Sensor was idle for a long time: previous reference is stale
            if (ref_us != 0 && (first - ref_us) > WIND_TIMEOUT_US) ref_us = 0;

            int64_t  start  = ref_us;
            uint32_t pulses = n;
            if (ref_us == 0) {          // no reference yet: use first pulse of this window
                start  = first;
                pulses = n - 1;
            }
            if (pulses > 0 && last > start) {
                rpm = ((float)pulses / WIND_PULSES_PER_REV) * 60.0f * 1e6f
                      / (float)(last - start);
            }
            ref_us = last;
        }

        ESP_LOGI("WIND", "pulses: %lu | RPM: %.1f", (unsigned long)n, rpm);
        if ( rpm < 1)
        {
            wind_speed =0;
        }
        else{
        wind_speed= (rpm-0.7306)/46.373;
        }
        if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            shared_data.Wind_speed = wind_speed;
            xSemaphoreGive(data_mutex);
        }
    }
}

static void Update(Data update_data,ssd1680_t *disp,ssd1680_color_t color)
{
    ssd1680_font_t *font = &font_terminal_14pt;
    char temperature_string[32];
    char humidity_string[32];
    char lux_string[32];
    char Wind_speed_string[32];
    snprintf(Wind_speed_string,sizeof(Wind_speed_string), "Wind:%.1f ms/s", update_data.Wind_speed);
    snprintf(temperature_string,sizeof(temperature_string),"Temp:%.1f C",update_data.Temperatuur);
    snprintf(humidity_string,sizeof(humidity_string),"Humi: %.1f %%",update_data.Humidity);


    snprintf(lux_string,sizeof(lux_string),"Light:%.1f L",update_data.Lux);
    if (update_data.Lux < 100)
    {
        ssd1680_set_area(disp, 0, 60, 0 + image_logo_1_100_60.width - 1, 60 + image_logo_1_100_60.height - 1, (uint8_t *)&image_logo_1_100_60.data, image_logo_1_100_60.data_size, SSD1680_BLACK, SSD1680_REVERSE_FALSE, SSD1680_REVERSE_TRUE);
    }
	else{
        ssd1680_set_area(disp, 0, 35, 0 + image_sun_1_100_100.width - 1, 35 + image_sun_1_100_100.height - 1, (uint8_t *)&image_sun_1_100_100.data, image_sun_1_100_100.data_size, SSD1680_BLACK, SSD1680_REVERSE_FALSE, SSD1680_REVERSE_TRUE);
    }
    ssd1680_draw_line(disp,120,0,120,128,color);
    ssd1680_draw_line(disp,121,0,121,128,color);
    ssd1680_draw_line(disp,122,0,122,128,color);

    ssd1680_display_string(disp, font, 0, 0,  update_data.Time,  color);
    ssd1680_display_string(disp, font, 0, 20, update_data.Place, color);
    ssd1680_display_string(disp,font,150,0,temperature_string,color);
    ssd1680_display_string(disp,font,150,30,humidity_string,color);
    ssd1680_display_string(disp,font,150,60,lux_string,color);
    ssd1680_display_string(disp,font,150,90,Wind_speed_string,color);
    ssd1680_send_framebuffer(disp);
    ssd1680_refresh(
        disp,
        FAST_FULL_REFRESH
    );
}



static void DHT(void *pvParameters)
{
    ESP_LOGI(TAG, "DHT task started");    
    float temperature = 0.0f;
    float humidity = 0.0f;

    gpio_set_pull_mode(DHT_GPIO_PIN,GPIO_PULLUP_ONLY);
    while (1)
    { ESP_LOGI(TAG, "DHT: reading..."); 
       esp_err_t res = dht_read_float_data(SENSOR_TYPE, DHT_GPIO_PIN, &humidity, &temperature);
       vTaskDelay(pdMS_TO_TICKS(500));
ESP_LOGI(TAG, "DHT: read returned %s (0x%x)", esp_err_to_name(res), res);
        if (res == ESP_OK)
        {
            ESP_LOGI(TAG,"DHT: Humidity %.1f%% | Temperature %.1f C",humidity,temperature);
            if (
                xSemaphoreTake(data_mutex,pdMS_TO_TICKS(100)) == pdTRUE)
            {
                shared_data.Temperatuur = temperature;
                shared_data.Humidity = humidity;
                shared_data.Dht_valid = true;

                xSemaphoreGive(data_mutex);
            }
        }
        else
        {
            ESP_LOGE(TAG,"DHT read failed: 0x%x",res);
        }



        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void ADC(void *pvParameters)
{
    int adc_value;

    adc_oneshot_unit_handle_t adc_handle;




    adc_oneshot_unit_init_cfg_t init_config =
    {
        .unit_id = ADC_UNIT,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,};


    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config,&adc_handle));




    adc_oneshot_chan_cfg_t config =
    {
        .bitwidth = ADC_BITWIDTH,
        .atten = ADC_ATTEN,
    };


    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle,ADC_PIN,&config));

    while (1)
    {

        ESP_ERROR_CHECK(
            adc_oneshot_read(adc_handle,ADC_PIN,&adc_value));

        float volts = ((float)adc_value / 4095.0f) * 3.3f;
        float uAmps = (volts / 10000.0f) * 1000000.0f;
        float lux = uAmps / 0.5f;
        ESP_LOGI("LUX","ADC: %d | Voltage: %.3f V | Lux: %.2f",adc_value,volts,lux);

        if (xSemaphoreTake(data_mutex,pdMS_TO_TICKS(100)) == pdTRUE)
        {
            shared_data.Lux = lux;
            xSemaphoreGive(data_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void WIFI(void *pvParameter)
{
    bool have_place = false;
    while (true) {
        fetch_time();
        if (!have_place) have_place = fetch_place();
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

static void E_ink(void *pvParameters)
{
    ssd1680_t *ssd1680_disp;

    spi_host_device_t spi_host = EPAPER_HOST;

    ssd1680_pinmap_t ssd1680_pinmap =
    {
        .busy = PIN_NUM_BCKL,
        .reset = PIN_NUM_RST,
        .dc = PIN_NUM_DC,
        .cs = PIN_NUM_CS
    };

    spi_bus_config_t buscfg =
    {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,

        .quadwp_io_num = -1,
        .quadhd_io_num = -1,

        .max_transfer_sz = 8192,

        .flags = SPICOMMON_BUSFLAG_MASTER,};

    ESP_LOGI(TAG,"Initializing SPI...");

    ESP_ERROR_CHECK(spi_bus_initialize(spi_host,&buscfg,SPI_DMA_CH_AUTO));

    uint8_t ssd1680_orientation = SSD1680_90_DEG;

    ESP_LOGI(TAG,"Initializing e-paper...");

    ssd1680_disp = ssd1680_init(spi_host,ssd1680_pinmap,EPAPER_RES_X,EPAPER_RES_Y,ssd1680_orientation);

    if (ssd1680_disp == NULL)
    {
        ESP_LOGE(TAG,"Failed to initialize e-paper");

        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG,"E-paper initialized");

    Data data;
    while (1)
    {
        if (
            xSemaphoreTake(data_mutex,pdMS_TO_TICKS(100)) == pdTRUE)
        {
            data = shared_data;
            xSemaphoreGive(data_mutex);
        }

        ESP_LOGI(TAG,"Display: Temp %.1f C | Hum %.1f %% | Lux %.1f",data.Temperatuur,data.Humidity,data.Lux);

        Update(data,ssd1680_disp,SSD1680_BLACK);
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}



void app_main(void)
{

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    wifi_init();
    data_mutex = xSemaphoreCreateMutex();


    if (data_mutex == NULL)
    {
        ESP_LOGE(TAG,"Failed to create data mutex");
        return;
    }

    homestation_mqtt_start();

    xTaskCreate(DHT,"DHT",4096,NULL,10,NULL);
   //BaseType_t ok = xTaskCreate(DHT, "DHT", 4096, NULL, 5, NULL);

    xTaskCreate(ADC,"ADC",4096,NULL,3,NULL);
    xTaskCreate(E_ink,"E_INK",4096,NULL,3,NULL);
    xTaskCreate(WIFI,"Wifi",4096,NULL,3,NULL);
    xTaskCreate(Wind_speed,"Wind",4096,NULL,3,NULL);
    xTaskCreate(MQTT,"MQTT",5120,NULL,3,NULL);

}