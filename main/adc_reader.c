#include <stdio.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include <driver/adc.h>
#include <driver/dac.h>
#include <esp_adc_cal.h>
#include <string.h>
#include "adc_reader.h"
#include "mqtt.h"
#include "freertos/task.h"

#define POWER_PIN 21  // GPIO 21, P12 from LoPy4
#define BATTERY_ADC_GAIN 3 // We meassure a fraction of the battery voltage
#define eos(s) ((s) + strlen(s))

// define number of ADCs to read, and its indices
#define N_ADC 3 // ADC channels used
#define N_ADC_MEASURES 2 // number of ADCs used for own measures (different
                        //channels could be used to calculate the same measure)

#define N_BROKER_SENDERS 1 // number independent sender timers

/* BIAS for the measuring circuit
 * bias = vdd * dac_value / 255
 * computation in mv to avoid floating point
 */
#define VDD  3300 // mv
#define BIAS 500  // mv
#define BIAS_DAC_VALUE (((BIAS * 255) + VDD/2)/ VDD)
#define DAC_CHANNEL DAC_CHANNEL_1

#define ADC_VREF 1100
#define ADC_ATTENUATION ADC_ATTEN_DB_11

// INFLUXDB Format
#define MAX_INFLUXDB_FIELDS 4
#define MAX_INFLUXDB_STRING 128
#define INFLUXDB_MEASUREMENT "cabahla"
#define INFLUXDB_LOCATION ",id=v1-n6"
#define FIELD_IRRADIATION "irradiation"
#define FIELD_BATTERY "battery"

#define MQTT_TOPIC "/ciu/lopy4/irradiation/1"

#define IRRADIATION_ADC_INDEX 0
#define BATTERY_ADC_INDEX     1
#define BIAS_ADC_INDEX        2 /* used for irradiation, always keep it with the
                                 * highest index
								 **/
#define NFIELDS 2 // number of entries of adc_params to send to the MQTT broker

static const char *TAG = "adc_reader";

static int get_adc_mv(int *value, int adc_index);
static int get_irradiation_mv(int *value, int adc_index);
static int get_battery_mv(int *value, int adc_index);
static void sampling_timer_callback(void *);
static void broker_sender_callback(void *);

struct adc_config_params {
    int window_size;
    int sample_frequency;
    int send_frenquency;
    int n_samples;
    int channel;
    char *influxdb_field;
    esp_adc_cal_characteristics_t adc_chars;
    int (*get_mv)(int *, int);
    int last_mean;
};

struct send_sample_buffer {
    int ini;
    int cont;
    int *samples;
    char payload[20];
};

// inizialise for each adc its parameters
static struct adc_config_params adc_params[N_ADC] = {
    // solar panel params
    {
        .window_size = CONFIG_WINDOW_SIZE_IRRAD,
        .sample_frequency = CONFIG_SAMPLE_FREQ_IRRAD,
        .send_frenquency = CONFIG_SEND_FREQ_IRRAD,
        .n_samples = CONFIG_N_SAMPLES_IRRAD,
        .channel = ADC1_CHANNEL_0,
        .influxdb_field = FIELD_IRRADIATION,
        .get_mv = get_irradiation_mv,
        .last_mean = 0,

    },
    // battery params
    {
        .window_size = CONFIG_WINDOW_SIZE_BATTERY,
        .sample_frequency = CONFIG_SAMPLE_FREQ_BATTERY,
        .send_frenquency = CONFIG_SEND_FREQ_BATTERY,
        .n_samples = CONFIG_N_SAMPLES_BATTERY,
        .channel = ADC1_CHANNEL_1,
        .influxdb_field = FIELD_BATTERY,
        .get_mv = get_battery_mv,
        .last_mean = 0,

    },
    // bias params. Many of its parameters are not used, it is used to calculate irradiation value
    {
        .window_size = CONFIG_WINDOW_SIZE_IRRAD,
        .sample_frequency = CONFIG_SAMPLE_FREQ_IRRAD,
        .send_frenquency = CONFIG_SEND_FREQ_IRRAD,
        .n_samples = CONFIG_N_SAMPLES_IRRAD,
        .channel = ADC1_CHANNEL_6,
        .influxdb_field = "",
        .get_mv = get_adc_mv,
        .last_mean = 0,
    },
};

static struct send_sample_buffer adcs_send_buffers[N_ADC_MEASURES] = {
    {
        .ini = 0,
        .cont = 0,
    },
    {
        .ini = 0,
        .cont = 0,
    },
};

static esp_timer_handle_t sampling_timer[N_ADC_MEASURES];
static esp_timer_create_args_t sample_timer_args[] = {
    {
        .callback = &sampling_timer_callback,
        .name = "sampling_timer_irra_adc",
        .arg = (void *) IRRADIATION_ADC_INDEX,
    },
   {
        .callback = &sampling_timer_callback,
        .name = "sampling_timer_battery_adc",
        .arg = (void *) BATTERY_ADC_INDEX,
    },
};

static esp_timer_handle_t broker_sender_timer[N_BROKER_SENDERS];
static esp_timer_create_args_t broker_sender_timer_args[N_BROKER_SENDERS] = {
    {
        .callback = &broker_sender_callback,
        .name = "broker_timer_adc",
        .arg = (void *) NFIELDS,
    },
};

static esp_err_t power_pin_setup(void)
{
    gpio_config_t io_conf;

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << POWER_PIN);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    return gpio_config(&io_conf);
}

static esp_err_t power_pin_down(void)
{
    return gpio_set_level(POWER_PIN, 0);
}

void adc_reader_deepsleep(void)
{
#ifdef CONFIG_SHUT_DOWN_POWER_PIN_DEEPSLEEP
	power_pin_down();
#endif
}

static esp_err_t power_pin_up(void)
{
    return gpio_set_level(POWER_PIN, 1);
}

static int get_adc_mv(int *value, int adc_index)
{
    int adc_val = adc1_get_raw(adc_params[adc_index].channel);
    *value = esp_adc_cal_raw_to_voltage(adc_val, &adc_params[adc_index].adc_chars);
    return 0;
}

static int get_battery_mv(int *value, int adc_index)
{
    get_adc_mv(value, IRRADIATION_ADC_INDEX);
	*value *= BATTERY_ADC_GAIN;
	return 0;
}

static int get_irradiation_mv(int *value, int adc_index)
{
    int panel_mv, bias_mv;
    get_adc_mv(&panel_mv, IRRADIATION_ADC_INDEX);
    get_adc_mv(&bias_mv, BIAS_ADC_INDEX);
    *value = panel_mv - bias_mv;
    return 0;
}

static void sampling_timer_callback(void * args)
{
    int adc_index = (int) args;

    int data, sample = 0;
#ifdef CONFIG_SHUT_DOWN_POWER_PIN
    if (adc_index == IRRADIATION_ADC_INDEX) {
        power_pin_up();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#endif
    for(int i= 0 ; i < adc_params[adc_index].n_samples; i++){
        if (adc_params[adc_index].get_mv(&data, adc_index))
            ESP_LOGE(TAG, "Error reading ADC with index %d", adc_index);
        else
            sample += data;
    }
    sample = (int) sample / adc_params[adc_index].n_samples;
#ifdef CONFIG_SHUT_DOWN_POWER_PIN
    if (adc_index == IRRADIATION_ADC_INDEX)
        power_pin_down();
#endif
    ESP_LOGI(TAG, "Sample from ADC(%d) = %d", adc_index, sample);

    //Save the taken sample in the circular buffer
	adcs_send_buffers[adc_index].samples[adcs_send_buffers[adc_index].cont %
		adc_params[adc_index].window_size] = sample;
    adcs_send_buffers[adc_index].cont++;
}

static char* buildInfluxDBString(int nfields)
{
    char* str = (char*) malloc(MAX_INFLUXDB_STRING);
    sprintf(str,INFLUXDB_MEASUREMENT);
    sprintf(eos(str),INFLUXDB_LOCATION);
    sprintf(eos(str)," "); // space between TAGS and FILEDS
    for (int i=0; i < (nfields-1); i++ )
        sprintf(eos(str),"%s=%d,",adc_params[i].influxdb_field,adc_params[i].last_mean);

    // last field without comma
    sprintf(eos(str),"%s=%d ", adc_params[nfields-1].influxdb_field, adc_params[nfields-1].last_mean);

    // TODO include timestamp in nanoseconds
	struct timeval tv_now;
	gettimeofday(&tv_now, NULL);
	int64_t time_ns = ((int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec) * 1000;
    sprintf(eos(str),"%lld", time_ns);

    return str;
}

static void broker_sender_callback(void * args)
{
    int adc_index = (int) args;
    int nsamples;
    int i;
    char* payload;

    //See if there are samples to send
    for (i=0; i < adc_index; i++) {
        if (adcs_send_buffers[i].cont > 0) {
            int mean = 0;
			nsamples  = (adcs_send_buffers[i].cont > adc_params[i].window_size)
				? adc_params[i].window_size : adcs_send_buffers[i].cont;
            // made the sample mean
            for (int j = 0; j < nsamples; j++)
                mean += adcs_send_buffers[i].samples[j];

            mean = mean / nsamples;
            adc_params[i].last_mean =mean;
            adcs_send_buffers[i].cont = 0;
        }
        // Keep last mean value if no new samples in buffer
        //else {
        //    adc_params[i].last_mean =-1;
        //}
    }

    if ((payload = buildInfluxDBString(adc_index)) == NULL) {
        ESP_LOGW(TAG, "There are still not data to send\n");
        return;
    }

    //sprintf(adcs_send_buffers[*adc_index].payload, "%d", mean);
    ESP_LOGI(TAG, "Send it to the broker: %s \n", payload);
    mqtt_send_data(MQTT_TOPIC, payload, 0, 1, 0);
    free(payload);
}

static esp_err_t set_bias(void)
{
    dac_output_enable(DAC_CHANNEL);
    return dac_output_voltage(DAC_CHANNEL, BIAS_DAC_VALUE);
}

static
int adc1_channel_setup(uint32_t channel, esp_adc_cal_characteristics_t *chars)
{
    int ret = 0;
    ret |= adc1_config_channel_atten(channel, ADC_ATTENUATION);
	ret |= esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTENUATION,
			ADC_WIDTH_BIT_12, ADC_VREF, chars);
    return ret;
}

static int adcs_setup(void)
{
    int ret = 0;
    adc1_config_width(ADC_WIDTH_BIT_12);

    for(int i = 0; i < N_ADC; i++)
		ret |= adc1_channel_setup(adc_params[i].channel,
				&adc_params[i].adc_chars);
    return ret;
}

static int change_sample_frequency(int sample_freq, int adc)
{
    if (esp_timer_stop(sampling_timer[adc]))
        return 1;

    vTaskDelay(pdMS_TO_TICKS(500));

    adc_params[adc].sample_frequency = sample_freq;

    if (esp_timer_start_periodic(sampling_timer[adc], adc_params[adc].sample_frequency*1000000))
        return 1;

    ESP_LOGI(TAG, "Changed sample frequency to %d s in ADC %d", sample_freq, adc);
    return 0;
}

static int change_broker_sender_frequency(int send_freq, int adc) {
    if (esp_timer_stop(broker_sender_timer[adc]))
        return 1;

    vTaskDelay(pdMS_TO_TICKS(500));

    adc_params[adc].send_frenquency = send_freq;

    if (esp_timer_start_periodic(broker_sender_timer[adc], adc_params[adc].send_frenquency*1000000))
        return 1;

    ESP_LOGI(TAG, "Changed broker send frequency to %d s in ADC %d", send_freq, adc);
    return 0;
}

static int change_sample_number(int n_samples, int adc) {
    if (esp_timer_stop(sampling_timer[adc]))
        return 1;

    vTaskDelay(pdMS_TO_TICKS(500));

    adc_params[adc].n_samples = n_samples;

    if (esp_timer_start_periodic(sampling_timer[adc], adc_params[adc].sample_frequency*1000000))
        return 1;

    ESP_LOGI(TAG, "Changed sample number to %d in ADC %d", n_samples, adc);
    return 0;
}

/*******************************************************************************
 * Public functions
 *******************************************************************************/

void adc_reader_update(char *topic, char *data)
{
	int value = atoi(data);
	int adc;
	char *target;
	if (strstr(topic, "irradiation")) {
		adc = IRRADIATION_ADC_INDEX;
		target = "irradiation";
	} else if (strstr(topic, "battery_level")) {
		adc = BATTERY_ADC_INDEX;
		target = "battery";
	} else {
		ESP_LOGI(TAG, "Unrecognized topic %s", topic);
		return;
	}

	if (strstr(topic, "sample_frequency") != NULL){
		change_sample_frequency(value, adc);
		ESP_LOGI(TAG, "Change %s sample frequency to %d s", target, value);
	} else if (strstr(topic, "send_frequency")) {
		change_broker_sender_frequency(value, adc);
		ESP_LOGI(TAG, "Change %s send frequency to %d s", target, value);
	} else if (strstr(topic, "sample_number")) {
		change_sample_number(value, adc);
		ESP_LOGI(TAG, "Change number of %s samples to %d.", target, value);
	}
}

int adc_reader_setup(void)
{
    struct timeval tv_now;
	int64_t delta_ms;

    // allocate memory for send buffers
    for(int i = 0; i < N_ADC_MEASURES; i++)
        adcs_send_buffers[i].samples = malloc(sizeof(int) * adc_params[i].window_size);

    //power pin configuration
    if(power_pin_setup() != ESP_OK || power_pin_up() != ESP_OK) {
        ESP_LOGE(TAG, "Failed configuring power pin.");
        return 1;
    }

    // DAC configuration
    if(set_bias() != ESP_OK) {
        ESP_LOGE(TAG, "Failed configuring DAC.");
        return 1;
    }

    // ADCs configuration
    if(adcs_setup() != ESP_OK) {
        ESP_LOGE(TAG, "Failed configuring ADCs.");
        return 1;
    }

    gettimeofday(&tv_now, NULL);
    delta_ms = (60 - (int64_t)(tv_now.tv_sec%60))* 1000 -  ((int64_t)tv_now.tv_usec/1000);
	if (delta_ms < 0)
		delta_ms = 0;
    ESP_LOGI(TAG, "Wait for delta_ms before programming timers: %li ms ", (long)delta_ms);
    vTaskDelay(delta_ms / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Done. Now programming timers");

    // timers configuration
    for(int i = 0; i < N_ADC_MEASURES; i++)
        esp_timer_create(&sample_timer_args[i], &sampling_timer[i]);

	ESP_LOGD(TAG, "Inicialazing broker sender timers\n");
    for(int i = 0; i < N_BROKER_SENDERS; i++)
		esp_timer_create(&broker_sender_timer_args[i], &broker_sender_timer[i]);

    return 0;
}

int adc_reader_start_sample_timers(void)
{
    int ret = 0;

    for(int i = 0; i < N_ADC_MEASURES; i++)
        ret |= esp_timer_start_periodic(sampling_timer[i], adc_params[i].sample_frequency*1000000);

    return ret;
}

int adc_reader_stop_sample_timers(void)
{
    int ret = 0;
    for(int i = 0; i < N_ADC_MEASURES; i++)
        ret |= esp_timer_stop(sampling_timer[i]);

    return ret;
}

int adc_reader_start_send_timers(void)
{
    int ret = 0;

    for(int i = 0; i < N_BROKER_SENDERS; i++)
        ret |= esp_timer_start_periodic(broker_sender_timer[i], adc_params[i].send_frenquency*1000000);

    return ret;
}

int adc_reader_stop_send_timers(void)
{
    int ret = 0;
    for(int i = 0; i < N_BROKER_SENDERS; i++)
        ret |= esp_timer_stop(broker_sender_timer[i]);

    return ret;
}
