#include "frame_net/net.h"

#include "frame_core/event.h"
#include "frame_log/log.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "net";

static frame_net_status_t s_status;
static SemaphoreHandle_t  s_lock;
static bool               s_inited;
static bool               s_time_valid;
static esp_timer_handle_t s_reconnect_timer;
static bool               s_want_connected;

#define RECONNECT_DELAY_MS 5000u

/* A disconnect is handled from the default event loop, which must not sleep:
 * IP events for every interface run through it. Retry from a one-shot timer. */
static void reconnect_cb(void *arg)
{
    (void)arg;
    if (s_want_connected) {
        esp_wifi_connect();
    }
}

static void schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = reconnect_cb,
            .name = "frame_wifi_retry",
        };
        if (esp_timer_create(&args, &s_reconnect_timer) != ESP_OK) {
            return;
        }
    }
    (void)esp_timer_stop(s_reconnect_timer);
    (void)esp_timer_start_once(s_reconnect_timer, (uint64_t)RECONNECT_DELAY_MS * 1000u);
}

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}
static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static void set_state(frame_net_state_t st)
{
    lock();
    frame_net_state_t prev = s_status.state;
    s_status.state = st;
    unlock();

    if (prev != st) {
        frame_event_post_simple(
            (st == FRAME_NET_CONNECTED) ? FRAME_EVT_NETWORK_UP : FRAME_EVT_NETWORK_DOWN,
            (int32_t)st, 0);
        FRAME_LOGI(TAG, "network -> %d", (int)st);
    }
}

/* ------------------------------------------------------------------ */
/* Event handlers                                                       */
/* ------------------------------------------------------------------ */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        set_state(FRAME_NET_CONNECTING);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        lock();
        s_status.disconnects++;
        unlock();
        set_state(FRAME_NET_DOWN);
        schedule_reconnect();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *evt = (const ip_event_got_ip_t *)data;
        lock();
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&evt->ip_info.ip));
        unlock();
        set_state(FRAME_NET_CONNECTED);
        frame_net_time_sync();
    }
}

/* ------------------------------------------------------------------ */

esp_err_t frame_net_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (sta == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL),
        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL),
        TAG, "ip handler");

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");

    s_inited = true;
    s_status.state = FRAME_NET_DOWN;
    ESP_LOGI(TAG, "network stack ready");
    return ESP_OK;
}

esp_err_t frame_net_connect(const char *ssid, const char *pass)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1u);
    if (pass != NULL) {
        strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1u);
    }

    lock();
    strncpy(s_status.ssid, ssid, sizeof(s_status.ssid) - 1u);
    s_status.ssid[sizeof(s_status.ssid) - 1u] = '\0';
    unlock();

    s_want_connected = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return ESP_OK;
}

esp_err_t frame_net_disconnect(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    s_want_connected = false;
    if (s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
    }
    esp_err_t err = esp_wifi_disconnect();
    set_state(FRAME_NET_DOWN);
    return err;
}

void frame_net_get_status(frame_net_status_t *out)
{
    if (out == NULL) {
        return;
    }
    lock();
    *out = s_status;
    unlock();

    wifi_ap_record_t rec;
    if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
        out->rssi = rec.rssi;
    }
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const frame_http_req_t *req;
    int   status;
    bool  header_done;
} http_ctx_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
    if (ctx == NULL || ctx->req == NULL) {
        return ESP_OK;
    }

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (ctx->req->on_chunk != NULL && evt->data != NULL && evt->data_len > 0) {
            ctx->req->on_chunk(ctx->req->user, (const uint8_t *)evt->data,
                               (size_t)evt->data_len);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t frame_http_perform(const frame_http_req_t *req)
{
    if (req == NULL || req->url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    http_ctx_t ctx = { .req = req, .status = 0, .header_done = false };

    esp_http_client_config_t cfg = {
        .url = req->url,
        .event_handler = http_event,
        .user_data = &ctx,
        .timeout_ms = (req->timeout_ms > 0) ? req->timeout_ms : 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .skip_cert_common_name_check = false,
        .follow_redirects = req->follow_redirects,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (req->body != NULL && req->body_len > 0u) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type",
                                   req->content_type ? req->content_type
                                                     : "application/octet-stream");
        esp_http_client_set_post_field(client, (const char *)req->body,
                                       (int)req->body_len);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err == ESP_OK) {
        ctx.status = status;
    } else {
        ctx.status = -1;
        FRAME_LOGW(TAG, "http %s failed: %s", req->url, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);

    if (req->cb != NULL) {
        req->cb(req->user, ctx.status, NULL, 0u);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Time                                                                 */
/* ------------------------------------------------------------------ */

static void on_sntp_sync(struct timeval *tv)
{
    (void)tv;
    s_time_valid = true;
    ESP_LOGI(TAG, "time synchronised");
}

esp_err_t frame_net_time_sync(void)
{
    if (s_time_valid) {
        return ESP_OK;
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(on_sntp_sync);
    esp_sntp_init();
    return ESP_OK;
}

bool frame_net_time_valid(void)
{
    return s_time_valid;
}

/* ------------------------------------------------------------------ */

esp_err_t frame_net_start_provisioning(char *ssid_out, size_t cap)
{
    /* Wi-Fi provisioning is available as a managed component
     * (network_provisioning). It is deliberately not linked in v1: the device
     * already exposes a full HTTP API for setup, and pulling in the provisioning
     * stack costs flash that the app slots need. */
    if (ssid_out != NULL && cap > 0u) {
        ssid_out[0] = '\0';
    }
    return ESP_ERR_NOT_SUPPORTED;
}
