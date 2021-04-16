/* MQTT over SSL Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "mqtt.h"

static const char *TAG = "MQTTS";


#if CONFIG_BROKER_CERTIFICATE_OVERRIDDEN == 1
static const uint8_t mqtt_eclipse_org_pem_start[]  = "-----BEGIN CERTIFICATE-----\n" CONFIG_BROKER_CERTIFICATE_OVERRIDE "\n-----END CERTIFICATE-----";
#else
extern const uint8_t mqtt_eclipse_org_pem_start[]   asm("_binary_mqtt_eclipse_org_pem_start");
#endif
extern const uint8_t mqtt_eclipse_org_pem_end[]   asm("_binary_mqtt_eclipse_org_pem_end");


extern void inicializarTimerSensorTemperaturaYHumedad();
extern void inicializarTimerSensorCO2(void);
extern void activarTimerSensorTemp();
extern void activarTimerSensorCO2();
extern void pararTimerSensorTemp();
extern void pararTimerSensorCO2();
extern void start_server_http();
extern void modificaSampleFreqCO2(int result);
extern void modificaSendFreqCO2(int result);
extern void modificaNSamplesCO2(int result);
extern void modificaSampleFreqTemp(int result);
extern void modificaSendFreqTemp(int result);
extern void modificaNSamplesTemp(int result);
extern void iniciarBLE(void);
extern void modificaScanFreq(int scan_freq);
extern void modificaTimeScan(int time_scan);

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    client = event->client;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

            if (primeraConexion){
                /*Inicializamos los buses I2c e iniciamos los timers de lectura y envio*/
                /*Iniciamos el BLE y el server HTTP*/
                start_server_http();
                vTaskDelay(pdMS_TO_TICKS(1000));
                iniciarBLE();
                inicializarTimerSensorTemperaturaYHumedad();
                inicializarTimerSensorCO2();
                primeraConexion = false;
                esp_mqtt_client_subscribe(client, TOPIC_SAMPLE_FREQ_CO2, 1);
                esp_mqtt_client_subscribe(client, TOPIC_SEND_FREQ_CO2, 1);
                esp_mqtt_client_subscribe(client, TOPIC_N_SAMPLES_CO2, 1);
                esp_mqtt_client_subscribe(client, TOPIC_SAMPLE_FREQ_TEMP, 1);
                esp_mqtt_client_subscribe(client, TOPIC_SEND_FREQ_TEMP, 1);
                esp_mqtt_client_subscribe(client, TOPIC_N_SAMPLES_TEMP, 1);
            } else{
                /*Activamos los timers de envio de los sensores*/
                activarTimerSensorTemp();
                activarTimerSensorCO2();
            }
            

            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            /*Paramos los envios de los datos de los sensores*/
            pararTimerSensorCO2();
            pararTimerSensorTemp();
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            event->data[event->data_len] = '\0'; //Necesario para que no sea el buffer más grande y añada 0 que no queremos al usar atoi()
            if (strstr(event->topic, "SAMPLE_FREQ") != NULL) {
                if (strstr(event->topic, "TEMP")){
                    modificaSampleFreqTemp(atoi(event->data));
                    ESP_LOGI(TAG, "Recibido un cambio de SAMPLE_FREQ a %d segundos para TEMP.", atoi(event->data));
                } else if (strstr(event->topic, "CO2")){
                    modificaSampleFreqCO2(atoi(event->data));
                    ESP_LOGI(TAG, "Recibido un cambio de SAMPLE_FREQ a %d segundos para CO2.", atoi(event->data));
                }
            } else if (strstr(event->topic, "SEND_FREQ")){
                if (strstr(event->topic, "TEMP")){
                    modificaSendFreqTemp(atoi(event->data));
                    ESP_LOGI(TAG, "Recibido un cambio de SEND_FREQ a %d para TEMP.", atoi(event->data));
                } else if (strstr(event->topic, "CO2")){
                    modificaSendFreqCO2(atoi(event->data));
                    ESP_LOGI(TAG, "Recibido un cambio de SEND_FREQ a %d para CO2.", atoi(event->data));
                }
            } else if (strstr(event->topic, "N_SAMPLES")){
                if (strstr(event->topic, "TEMP")){
                    modificaNSamplesTemp(atoi(event->data));
                    ESP_LOGI(TAG, "Recibido un cambio de N_SAMPLES a %d para TEMP.", atoi(event->data));
                } else if (strstr(event->topic, "CO2")){
                    modificaNSamplesCO2(atoi(event->data));
                    ESP_LOGI(TAG, "Recibido un cambio de N_SAMPLES a %d para CO2.", atoi(event->data));
                }
            } else if (strstr(event->topic, "AFORO")){
                if (strstr(event->topic, "TIME_SCAN")){
                    modificaTimeScan(atoi(event->data));
                    ESP_LOGI(TAG, "Recibido un cambio de TIME_SCAN a %d.", atoi(event->data));
                } else if (strstr(event->topic, "FREQ_SCAN")){
                    modificaScanFreq(atoi(event->data));
                    ESP_LOGI(TAG, "Recibido un cambio de FREQ_SCAN a %d.", atoi(event->data));
                }
            }

            //printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            //printf("DATA=%.*s\r\n", event->data_len, event->data);

            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}


static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = BROKER_URI, //BROKER_URL
        .cert_pem = (const char *)mqtt_eclipse_org_pem_start,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

void enviar_al_brocker(const char *topic, char *data, int len, int qos, int retain){
    esp_mqtt_client_publish(client, topic, data, len, qos, retain);
}
    