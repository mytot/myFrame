/*
 * frame_net/net.h — Wi-Fi station with a reconnect policy and an HTTP wrapper.
 *
 * Everything here runs on core 0. Nothing in this component may call into
 * LVGL; results are delivered through callbacks that the caller marshals onto
 * the UI thread with frame_ui_dispatch().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_HTTP_STATUS_MAX 8u

typedef enum {
    FRAME_NET_DOWN = 0,
    FRAME_NET_CONNECTING,
    FRAME_NET_CONNECTED,
} frame_net_state_t;

typedef struct {
    frame_net_state_t state;
    int8_t            rssi;
    char              ip[16];
    char              ssid[33];
    uint32_t          disconnects;
} frame_net_status_t;

typedef void (*frame_http_cb_t)(void *user, int status, const uint8_t *body, size_t len);

/* ------------------------------------------------------------------ */

esp_err_t frame_net_init(void);

/* Non-blocking: brings the interface up and returns. State changes arrive as
 * FRAME_EVT_NETWORK_UP / FRAME_EVT_NETWORK_DOWN. */
esp_err_t frame_net_connect(const char *ssid, const char *pass);
esp_err_t frame_net_disconnect(void);
void      frame_net_get_status(frame_net_status_t *out);

/* ------------------------------------------------------------------ */
/* HTTP                                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *url;
    const char *content_type;   /* for POST  */
    const uint8_t *body;        /* for POST  */
    size_t      body_len;
    frame_http_cb_t cb;
    void       *user;
    int        timeout_ms;
    /* Streaming sink: when set, body chunks are delivered as they arrive and
     * `cb` is invoked once at the end with the final status. */
    void (*on_chunk)(void *user, const uint8_t *data, size_t len);
    bool    follow_redirects;
} frame_http_req_t;

esp_err_t frame_http_perform(const frame_http_req_t *req);

/* Time synchronisation; idempotent. TLS will fail without it. */
esp_err_t frame_net_time_sync(void);
bool      frame_net_time_valid(void);

/* ------------------------------------------------------------------ */
/* Provisioning                                                          */
/* ------------------------------------------------------------------ */

/* Sends the device into AP provisioning mode. Returns the SSID to join. */
esp_err_t frame_net_start_provisioning(char *ssid_out, size_t cap);

#ifdef __cplusplus
}
#endif
