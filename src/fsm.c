#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_system.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <esp_sntp.h>

#include "fsm.h"
#include "mqtt.h"
#include "app_prov.h"
#include "adc_reader.h"

#ifdef CONFIG_EXAMPLE_USE_SEC_1
#define PROV_SECURITY 1
#else
#define PROV_SECURITY 0
#endif

#define EXAMPLE_AP_RECONN_ATTEMPTS  CONFIG_EXAMPLE_AP_RECONN_ATTEMPTS

#define SNTP_SYNC_INTERVAL_MS (30 * 60 * 1000)
static int32_t HOUR_TO_SLEEP = CONFIG_HOUR_TO_SLEEP;
static int32_t HOUR_TO_WAKEUP = CONFIG_HOUR_TO_WAKEUP;

#define TAG "FSM"

// Boolean variables, the state is the actual combination of these variables
static bool provisioned = false;
static bool wifi_conn   = false;
static bool ntp_sync    = false;
static bool ntp_init    = false;
static bool mqtt_conn   = false;
static bool adc_config  = false;

static esp_timer_handle_t deep_sleep_timer;

/*******************************************************************************
 * Private functions
 *******************************************************************************/
static uint64_t hm2us(int hours, int minutes)
{
    uint64_t tm = (long long int)(minutes + 60*hours);
    uint64_t us = (long long int) 1000000 * 60 * tm;
	return us;
}

static int deep_sleep_wakeup_schedule(uint64_t *h, uint64_t  *m)
{
    time_t now;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
	int schedule = 1;

	if (timeinfo.tm_hour >= HOUR_TO_WAKEUP && timeinfo.tm_hour < HOUR_TO_SLEEP) {
    	ESP_LOGW(TAG, "durint uptime hours");
		schedule = 0;
	} else if (timeinfo.tm_hour < HOUR_TO_WAKEUP) {
    	ESP_LOGW(TAG, "after midnight");
    	*h = HOUR_TO_WAKEUP - timeinfo.tm_hour - 1;
		*m = 60 - timeinfo.tm_min;
	} else {
    	ESP_LOGI(TAG, "before midnight");
    	*h = 24 - 1 - timeinfo.tm_hour + HOUR_TO_WAKEUP;
		*m = 60 - timeinfo.tm_min;
	}
	return schedule;
}

static void go_deep_sleep(void * args)
{
	uint64_t us, h, m;

	if (deep_sleep_wakeup_schedule(&h, &m)) {
		us = hm2us(h, m);
		ESP_LOGI(TAG, "Deep sleep for %llu hours %llu minutes (%llu us)", h, m,
				us);
#ifdef CONFIG_SHUT_DOWN_POWER_PIN
		power_pin_down();
#endif
		esp_sleep_enable_timer_wakeup(us);
		/* Do enter deep sleep */
		esp_deep_sleep_start();
	}
}

static const esp_timer_create_args_t deep_sleep_timer_args = {
	.callback = &go_deep_sleep,
	.name = "deep_sleep_timer_cb"
};

static void wait_for_sntp_sync(void)
{
    ESP_LOGI(TAG, "Waiting for system time to be adjusted");
    if (sntp_get_sync_mode() == SNTP_SYNC_MODE_SMOOTH) {
        struct timeval outdelta;
    	ESP_LOGI(TAG, "SNTP smooth adjust mode");
        while (sntp_get_sync_status() == SNTP_SYNC_STATUS_IN_PROGRESS) {
            adjtime(NULL, &outdelta);
            ESP_LOGI(TAG, "Waiting for adjusting time ... outdelta = %li sec: %li ms: %li us",
                        (long)outdelta.tv_sec,
                        outdelta.tv_usec/1000,
                        outdelta.tv_usec%1000);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
    } else {
		int retry = 0;
		while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET &&
				++retry <= 10) {
			ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry,
					10);
			vTaskDelay(2000 / portTICK_PERIOD_MS);
		}
	}
    ESP_LOGI(TAG, "Time should be adjusted");
}

static void time_sync_notification_cb(struct timeval *tv)
{
    struct tm timeinfo;
    char strftime_buf[64];
    localtime_r(&tv->tv_sec, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI("NTP Event", "date/time received from ntp server is: %s",
			strftime_buf);
	fsm_ntp_sync();
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    static int s_retry_num = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_AP_RECONN_ATTEMPTS) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
			fsm_wifi_disconnected();
		}
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
		fsm_wifi_connected();
    }
}

#ifdef CONFIG_EXAMPLE_SSID
static char *get_provisioning_ssid(void)
{
	return strdup(CONFIG_EXAMPLE_SSID);
}
#else
static char *get_provisioning_ssid(void)
{
	char *ssid = malloc(33);
	if (ssid) {
		uint8_t mac[6];
		esp_wifi_get_mac(WIFI_IF_STA, mac);
		snprintf(ssid, 33, "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);
	}
	return ssid;
}
static void prov_free_ssid(char *ssid)
{
	free(ssid);
}
#endif

#ifdef CONFIG_EXAMPLE_USE_POP
static protocomm_security_pop_t *get_security_pop(void)
{
	protocomm_security_pop_t * pop;

	pop = malloc(sizeof(protocomm_security_pop_t));
	if (pop) {
		pop->data = (uint8_t *) CONFIG_EXAMPLE_POP;
		pop->len = (sizeof(CONFIG_EXAMPLE_POP)-1);
	}

	return pop;
}
#else
static protocomm_security_pop_t *get_security_pop(void)
{
	return NULL;
}
#endif

/*******************************************************************************
 * Events for the FSM
 *******************************************************************************/
void fsm_mqtt_disconnected(void)
{
	if (!mqtt_conn)
		ESP_LOGW(TAG, "MQTT disconnected event when not connected");
	mqtt_conn = false;
    adc_reader_stop_send_timers();
}

void fsm_mqtt_connected(void)
{
	if (mqtt_conn)
		ESP_LOGW(TAG, "MQTT connected event when already connected");
	mqtt_conn = true;

	if (!adc_config) {
		if(adc_reader_setup())
			ESP_LOGE(TAG, "Failed to create adc_reader module.");
		adc_config = true;
	}
	adc_reader_start_send_timers();
}

void fsm_ntp_sync(void)
{
	if (ntp_sync)
    	ESP_LOGI(TAG, "NTP event on synched system");
	else
    	ESP_LOGI(TAG, "NTP event on unsynched system");
	ntp_sync = true;

	wait_for_sntp_sync();

    char strftime_buf[64];
    time_t now;
    struct tm timeinfo;
    time(&now);
    setenv("TZ", "Europe/Madrid", 1);
    tzset();
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time in Madrid is: %s", strftime_buf);

	// Reschedule deep_sleep
	esp_timer_stop(deep_sleep_timer);
	int hours, mins;
	uint64_t us;
	if (timeinfo.tm_hour >= HOUR_TO_WAKEUP &&
			timeinfo.tm_hour < HOUR_TO_SLEEP) {
		hours = HOUR_TO_SLEEP - timeinfo.tm_hour - 1;
		mins  = 60 - timeinfo.tm_min;
		us = hm2us(hours, mins);
    	ESP_LOGI(TAG, "Schedule deep sleep on %d hours %d minutes (%lld us)",
				hours, mins, us);
    	esp_timer_create(&deep_sleep_timer_args, &deep_sleep_timer);
    	esp_timer_start_once(deep_sleep_timer, us);
	} else {
    	ESP_LOGI(TAG, "Time synched to deep sleep window: deep sleep right now!");
		go_deep_sleep(NULL);
	}

	if (!mqtt_conn)
		mqtt_app_start();
}

void fsm_wifi_connected(void)
{
	if (wifi_conn)
		ESP_LOGE(TAG, "Wifi connected event when already connected to wifi");
	wifi_conn = true;

	if (!ntp_init) {
		ESP_LOGI(TAG, "Initializing SNTP");
		sntp_setoperatingmode(SNTP_OPMODE_POLL);
		sntp_setservername(0, "pool.ntp.org");
		sntp_set_sync_interval(SNTP_SYNC_INTERVAL_MS);
		sntp_set_time_sync_notification_cb(time_sync_notification_cb);
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
		sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
#else
  		sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
#endif
		sntp_init();
		ntp_init = true;
	} else {
		sntp_restart();
	}
}

void fsm_wifi_disconnected(void)
{
	if (!wifi_conn) {
		ESP_LOGW(TAG, "Wifi disconnected event when not connected to wifi");
		return;
	}
	wifi_conn = false;
	ESP_LOGW(TAG, "Wifi disconnected event");
	//if (ntp_init) {
		//FIXME: can we stop the sntp?
	//}
	if (mqtt_conn) {
		mqtt_app_stop();
		mqtt_conn = false;
	}
	if (adc_config) {
		adc_reader_stop_send_timers();
	}
}

void fsm_provisioned(void)
{
	if (provisioned) {
		ESP_LOGE(TAG, "Provisioned event when already provisioned");

		/* Start WiFi station with credentials set during provisioning */
		ESP_LOGI(TAG, "Starting WiFi station");
		ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
					wifi_event_handler, NULL));
		ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
					wifi_event_handler, NULL));
		/* Start Wi-Fi in station mode with credentials set during provisioning */
		ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
		ESP_ERROR_CHECK(esp_wifi_start());
	} else {
		ESP_LOGE(TAG, "Provisioned event when not already provisioned");
		provisioned = true;
		fsm_wifi_connected(); // app_prov leaves the node already connected
	}
}

void fsm_init(void)
{
	provisioned = false;
	wifi_conn   = false;
	ntp_sync    = false;
	ntp_init    = false;
	mqtt_conn   = false;
	adc_config  = false;

    /* Initialize networking stack */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Create default event loop needed by the
     * main app and the provisioning service */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Initialize NVS needed by Wi-Fi */
    ESP_ERROR_CHECK(nvs_flash_init());

	/* Initialize Wi-Fi including netif with default config */
	esp_netif_create_default_wifi_sta();
	esp_netif_create_default_wifi_ap();
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	/* Check if device is provisioned */
	if (app_prov_is_provisioned(&provisioned) != ESP_OK) {
		ESP_LOGE(TAG, "Error getting device provisioning state");
		return;
	}

    if (!provisioned) {
		/* If not provisioned, start provisioning via soft AP */
		protocomm_security_pop_t *pop = get_security_pop();
		char *ssid = get_provisioning_ssid();
		ESP_ERROR_CHECK(app_prov_start_softap_provisioning(
					ssid, CONFIG_EXAMPLE_PASS, PROV_SECURITY, pop));
		free(ssid);
		free(pop);
	} else {
		ESP_LOGI(TAG, "Node is provisioned");
		fsm_provisioned();
	}
}
