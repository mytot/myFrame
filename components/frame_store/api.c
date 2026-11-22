/*
 * api.c — the device-side HTTP API the dev store (and curl) talks to.
 *
 * This is a development surface. It runs on port 80 with no authentication
 * because the threat model is "a laptop on the same bench"; it is compiled
 * only when CONFIG_FRAME_HTTP_API_ENABLE is set, and a release build should
 * leave it off. Nothing here bypasses package verification — uploads go
 * through exactly the same install path as a download.
 *
 * Blocking model: the cheap routes answer at once; an upload installs
 * synchronously on the httpd task (a dev convenience, so curl gets a real
 * status), and the long-running catalog/update/remove routes are queued to
 * the store task so the HTTP server never stalls for a flash erase.
 */
#include "frame_store/store.h"
#include "store_internal.h"

#include "frame_core/app.h"
#include "frame_core/event.h"
#include "frame_log/log.h"

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_FRAME_HTTP_API_ENABLE
#define CONFIG_FRAME_HTTP_API_ENABLE 1
#endif

#define API_MAX_UPLOAD (4u * 1024u * 1024u)
#define API_JSON_MAX   (96u * 1024u)
#define API_HTTPD_STACK 12288u

static const char *TAG = "api";
static httpd_handle_t s_server;

/* ------------------------------------------------------------------ */
/* JSON builder with proper escaping                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   overflow;
} jb_t;

static void jb_init(jb_t *b, char *buf, size_t cap)
{
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->overflow = false;
    if (cap > 0u) {
        buf[0] = '\0';
    }
}

static void jb_raw(jb_t *b, const char *s, size_t n)
{
    if (b->overflow) {
        return;
    }
    if (b->len + n + 1u > b->cap) {
        b->overflow = true;
        return;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

/* Appends a JSON string literal, escaping ", \\ and control characters. */
static void jb_str(jb_t *b, const char *s)
{
    jb_raw(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p != '\0'; p++) {
        char tmp[8];
        switch (*p) {
        case '"':  jb_raw(b, "\\\"", 2); break;
        case '\\': jb_raw(b, "\\\\", 2); break;
        case '\n': jb_raw(b, "\\n", 2); break;
        case '\r': jb_raw(b, "\\r", 2); break;
        case '\t': jb_raw(b, "\\t", 2); break;
        default:
            if (*p < 0x20u) {
                snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)*p);
                jb_raw(b, tmp, 6);
            } else {
                jb_raw(b, (const char *)p, 1);
            }
        }
        if (b->overflow) {
            break;
        }
    }
    jb_raw(b, "\"", 1);
}

static void jb_fmt(jb_t *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        jb_raw(b, tmp, (size_t)n);
    }
}

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                        */
/* ------------------------------------------------------------------ */

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 500: return "500 Internal Server Error";
    default:  return "500 Internal Server Error";
    }
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_status(req, status_text(200));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_error(httpd_req_t *req, int code, const char *msg)
{
    char body[256];
    jb_t b;
    jb_init(&b, body, sizeof(body));
    jb_raw(&b, "{\"error\":", 8);
    jb_str(&b, msg);
    jb_raw(&b, "}", 1);
    httpd_resp_set_status(req, status_text(code));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* Reads the whole body into a PSRAM buffer. Caller frees. */
static uint8_t *read_body(httpd_req_t *req, size_t *len_out)
{
    if (req->content_len == 0u || req->content_len > API_MAX_UPLOAD) {
        return NULL;
    }
    const size_t len = req->content_len;
    uint8_t *buf = heap_caps_malloc(len + 1u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(len + 1u);
    }
    if (buf == NULL) {
        return NULL;
    }
    size_t got = 0;
    while (got < len) {
        const int n = httpd_req_recv(req, (char *)buf + got, (size_t)(len - got) > 64u * 1024u
                                                               ? 64u * 1024u : (int)(len - got));
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            return NULL;
        }
        got += (size_t)n;
    }
    buf[len] = '\0';
    *len_out = len;
    return buf;
}

/* memmem is a GNU extension; a tiny local version keeps this portable. */
static const uint8_t *find_bytes(const uint8_t *hay, size_t hay_len,
                                 const uint8_t *needle, size_t needle_len)
{
    if (needle_len == 0u || hay_len < needle_len) {
        return NULL;
    }
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

/*
 * Pulls the first file part out of a multipart/form-data body. Returns a
 * pointer into `body` and sets *out_len. Falls back to the whole body when
 * the request is not multipart (raw .fpack upload).
 */
static const uint8_t *multipart_first_part(httpd_req_t *req, uint8_t *body, size_t len, size_t *out_len)
{
    char ctype[128];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) != ESP_OK ||
        strncmp(ctype, "multipart/form-data", 19) != 0) {
        *out_len = len;
        return body;
    }
    const char *b = strstr(ctype, "boundary=");
    if (b == NULL) {
        return NULL;
    }
    b += 9;
    while (*b == ' ') {
        b++;
    }
    /* Strip a surrounding quote pair if the client sent one. */
    char boundary[80];
    snprintf(boundary, sizeof(boundary), "--%s", b);
    size_t blen = strlen(boundary);
    if (blen > 3u && boundary[blen - 1u] == '"') {
        boundary[--blen] = '\0';
    }

    const uint8_t *p = find_bytes(body, len, (const uint8_t *)boundary, blen);
    if (p == NULL) {
        return NULL;
    }
    p += blen;
    const uint8_t *hdr_end = find_bytes(p, len - (size_t)(p - body), (const uint8_t *)"\r\n\r\n", 4);
    if (hdr_end == NULL) {
        return NULL;
    }
    hdr_end += 4;

    const uint8_t *end = find_bytes(hdr_end, len - (size_t)(hdr_end - body),
                                    (const uint8_t *)"\r\n--", 4);
    if (end == NULL || end < hdr_end) {
        return NULL;
    }
    *out_len = (size_t)(end - hdr_end);
    return hdr_end;
}

/* ------------------------------------------------------------------ */
/* GET /api/apps                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t h_apps_get(httpd_req_t *req)
{
    char *out = heap_caps_malloc(API_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) {
        return send_error(req, 500, "out of memory");
    }
    jb_t b;
    jb_init(&b, out, API_JSON_MAX);
    jb_raw(&b, "{\"apps\":[", 9);

    const size_t n = frame_app_manager_count();
    for (size_t i = 0; i < n && !b.overflow; i++) {
        const frame_app_state_t *a = frame_app_manager_at(i);
        if (a == NULL) {
            continue;
        }
        const char *kind = (a->kind == FRAME_APP_KIND_NATIVE) ? "native"
                         : (a->kind == FRAME_APP_KIND_MANAGED) ? "managed" : "builtin";
        jb_raw(&b, (i == 0u) ? "{" : ",{", 1);
        jb_raw(&b, "\"id\":", 5);      jb_str(&b, a->id);
        jb_raw(&b, ",\"name\":", 7);   jb_str(&b, a->name);
        jb_raw(&b, ",\"version\":", 10); jb_str(&b, a->version);
        jb_fmt(&b, ",\"kind\":\"%s\",\"builtin\":%s,\"slot\":%u,\"update\":%s,\"available\":",
               kind, a->builtin ? "true" : "false", (unsigned)a->slot,
               a->update_available ? "true" : "false");
        jb_str(&b, a->available_version);
        jb_raw(&b, "}", 1);
    }
    jb_fmt(&b, "],\"catalog\":{\"valid\":%s,\"count\":%u}}",
           frame_store_catalog_valid() ? "true" : "false",
           (unsigned)frame_store_catalog_count());
    esp_err_t err = b.overflow ? send_error(req, 500, "response too large")
                               : send_json(req, out);
    free(out);
    return err;
}

/* ------------------------------------------------------------------ */
/* POST /api/apps/install                                               */
/* ------------------------------------------------------------------ */

static esp_err_t h_install_post(httpd_req_t *req)
{
    if (req->content_len > API_MAX_UPLOAD) {
        return send_error(req, 413, "package larger than 4 MB");
    }
    size_t len = 0;
    uint8_t *body = read_body(req, &len);
    if (body == NULL) {
        return send_error(req, 400, "body unreadable");
    }
    size_t plen = 0;
    const uint8_t *part = multipart_first_part(req, body, len, &plen);
    if (part == NULL || plen == 0u) {
        free(body);
        return send_error(req, 400, "no package found in request");
    }

    esp_err_t err = frame_store_install_bytes(part, plen, NULL);
    free(body);
    if (err != ESP_OK) {
        return send_error(req, 400, esp_err_to_name(err));
    }
    return send_json(req, "{\"installed\":true}");
}

/* ------------------------------------------------------------------ */
/* POST /api/apps/<id>/update, /api/apps/<id>/uninstall                 */
/* ------------------------------------------------------------------ */

static esp_err_t queue_request(frame_store_cmd_t cmd, const char *id)
{
    frame_store_request_t r = { 0 };
    r.cmd = cmd;
    store_copy(r.id, sizeof(r.id), id);
    return frame_store_request(&r);
}

/* Extracts <id> from "/api/apps/<id>/<action>" into a bounded buffer. */
static bool path_app_id(httpd_req_t *req, const char *action, char *id_out, size_t cap)
{
    const char *uri = req->uri;
    const char *start = uri + strlen("/api/apps/");
    const char *slash = strchr(start, '/');
    if (slash == NULL || slash == start || (size_t)(slash - start) >= cap) {
        return false;
    }
    if (strcmp(slash + 1, action) != 0) {
        return false;
    }
    memcpy(id_out, start, (size_t)(slash - start));
    id_out[slash - start] = '\0';
    return id_out[0] != '\0';
}

static esp_err_t h_app_update_post(httpd_req_t *req)
{
    char id[FRAME_APP_ID_MAX];
    if (!path_app_id(req, "update", id, sizeof(id))) {
        return send_error(req, 404, "unknown route");
    }
    if (frame_app_manager_find(id) == NULL) {
        return send_error(req, 404, "not installed");
    }
    esp_err_t err = queue_request(FRAME_STORE_CMD_INSTALL, id);
    if (err != ESP_OK) {
        return send_error(req, 409, "store busy");
    }
    return send_json(req, "{\"queued\":true}");
}

static esp_err_t h_app_uninstall_post(httpd_req_t *req)
{
    char id[FRAME_APP_ID_MAX];
    if (!path_app_id(req, "uninstall", id, sizeof(id))) {
        return send_error(req, 404, "unknown route");
    }
    if (frame_app_manager_find(id) == NULL) {
        return send_error(req, 404, "not installed");
    }
    esp_err_t err = queue_request(FRAME_STORE_CMD_REMOVE, id);
    if (err != ESP_OK) {
        return send_error(req, 409, "store busy");
    }
    return send_json(req, "{\"queued\":true}");
}

/* ------------------------------------------------------------------ */
/* GET /api/catalog, POST /api/catalog/refresh                          */
/* ------------------------------------------------------------------ */

static esp_err_t h_catalog_get(httpd_req_t *req)
{
    char *out = heap_caps_malloc(API_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) {
        return send_error(req, 500, "out of memory");
    }
    jb_t b;
    jb_init(&b, out, API_JSON_MAX);
    jb_raw(&b, "{\"valid\":", 9);
    jb_raw(&b, frame_store_catalog_valid() ? "true" : "false", frame_store_catalog_valid() ? 4u : 5u);
    jb_raw(&b, ",\"store\":", 9);
    jb_str(&b, frame_store_url());
    jb_raw(&b, ",\"apps\":[", 9);

    const size_t n = frame_store_catalog_count();
    for (size_t i = 0; i < n && !b.overflow; i++) {
        const frame_catalog_entry_t *e = frame_store_catalog_at(i);
        if (e == NULL) {
            continue;
        }
        const frame_app_state_t *inst = frame_app_manager_find(e->manifest.id);
        const bool installed = (inst != NULL);
        const bool update = installed && inst->update_available;
        jb_raw(&b, (i == 0u) ? "{" : ",{", 1);
        jb_raw(&b, "\"id\":", 5);        jb_str(&b, e->manifest.id);
        jb_raw(&b, ",\"name\":", 7);     jb_str(&b, e->manifest.name);
        jb_raw(&b, ",\"version\":", 10); jb_str(&b, e->manifest.version);
        jb_fmt(&b, ",\"kind\":\"%s\",\"size\":%u,\"installed\":%s,\"update\":%s,\"description\":",
               e->manifest.kind == FRAME_MANIFEST_NATIVE ? "native" : "managed",
               (unsigned)e->package_size, installed ? "true" : "false", update ? "true" : "false");
        jb_str(&b, e->manifest.description);
        jb_raw(&b, ",\"author\":", 9);
        jb_str(&b, e->manifest.author);
        jb_raw(&b, "}", 1);
    }
    jb_raw(&b, "]}", 2);
    esp_err_t err = b.overflow ? send_error(req, 500, "response too large")
                               : send_json(req, out);
    free(out);
    return err;
}

static esp_err_t h_catalog_refresh_post(httpd_req_t *req)
{
    frame_store_request_t r = { .cmd = FRAME_STORE_CMD_REFRESH };
    esp_err_t err = frame_store_request(&r);
    if (err != ESP_OK) {
        return send_error(req, 409, "store busy");
    }
    return send_json(req, "{\"queued\":true}");
}

/* ------------------------------------------------------------------ */
/* GET /api/logs?level=&tag=&limit=                                     */
/* ------------------------------------------------------------------ */

static esp_err_t h_logs_get(httpd_req_t *req)
{
    char query[160] = { 0 };
    frame_log_level_t level = FRAME_LOG_DEBUG;
    char tag[FRAME_LOG_TAG_MAX] = { 0 };
    size_t max_lines = 200u;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[32];
        if (httpd_req_get_query_value(query, "level", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, "error") == 0)      level = FRAME_LOG_ERROR;
            else if (strcmp(val, "warn") == 0)  level = FRAME_LOG_WARN;
            else if (strcmp(val, "info") == 0)  level = FRAME_LOG_INFO;
        }
        if (httpd_req_get_query_value(query, "tag", tag, sizeof(tag)) != ESP_OK) {
            tag[0] = '\0';
        }
        if (httpd_req_get_query_value(query, "limit", val, sizeof(val)) == ESP_OK) {
            const long l = strtol(val, NULL, 10);
            if (l > 0 && l <= 1000) {
                max_lines = (size_t)l;
            }
        }
    }

    frame_log_line_t *lines = heap_caps_malloc(sizeof(frame_log_line_t) * max_lines,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (lines == NULL) {
        return send_error(req, 500, "out of memory");
    }
    const size_t n = frame_log_query(level, tag[0] ? tag : NULL, lines, max_lines);

    char *out = heap_caps_malloc(API_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) {
        free(lines);
        return send_error(req, 500, "out of memory");
    }
    jb_t b;
    jb_init(&b, out, API_JSON_MAX);
    jb_raw(&b, "{\"lines\":[", 10);
    for (size_t i = 0; i < n && !b.overflow; i++) {
        jb_fmt(&b, "%s{\"t\":%llu,\"level\":%d,\"tag\":", (i == 0u) ? "" : ",",
               (unsigned long long)lines[i].timestamp_us, (int)lines[i].level);
        jb_str(&b, lines[i].tag);
        jb_raw(&b, ",\"text\":", 7);
        jb_str(&b, lines[i].text);
        jb_raw(&b, "}", 1);
    }
    jb_raw(&b, "]}", 2);
    free(lines);
    esp_err_t err = b.overflow ? send_error(req, 500, "response too large")
                               : send_json(req, out);
    free(out);
    return err;
}

/* ------------------------------------------------------------------ */
/* POST /api/launcher/open/<id>                                         */
/* ------------------------------------------------------------------ */

static esp_err_t h_launcher_open_post(httpd_req_t *req)
{
    const char *id = req->uri + strlen("/api/launcher/open/");
    if (id[0] == '\0') {
        return send_error(req, 404, "missing app id");
    }
    for (size_t i = 0; i < frame_app_manager_count(); i++) {
        const frame_app_state_t *a = frame_app_manager_at(i);
        if (a != NULL && strcmp(a->id, id) == 0) {
            (void)frame_event_post_simple(FRAME_EVT_APP_LAUNCH, (int32_t)i, 0);
            return send_json(req, "{\"queued\":true}");
        }
    }
    return send_error(req, 404, "not installed");
}

/* ------------------------------------------------------------------ */
/* GET /api/info — identity, versions, memory (handy from the dev store) */
/* ------------------------------------------------------------------ */

static esp_err_t h_info_get(httpd_req_t *req)
{
    /* Kept deliberately tiny; the dev store uses it to detect that it is
     * talking to a Frame device before issuing any other request. */
    char out[320];
    snprintf(out, sizeof(out),
             "{\"frame\":true,\"sdk\":\"%s\",\"abi\":%u,\"store\":\"%s\",\"state\":\"%s\"}",
             FRAME_SDK_VERSION, (unsigned)FRAME_ABI_VERSION, frame_store_url(),
             frame_store_state_str(frame_store_state()));
    return send_json(req, out);
}

/* ------------------------------------------------------------------ */

static const httpd_uri_t k_uris[] = {
    { .uri = "/api/apps",             .method = HTTP_GET,  .handler = h_apps_get,            .user_ctx = NULL },
    { .uri = "/api/apps/install",     .method = HTTP_POST, .handler = h_install_post,        .user_ctx = NULL },
    { .uri = "/api/apps/*/update",    .method = HTTP_POST, .handler = h_app_update_post,     .user_ctx = NULL },
    { .uri = "/api/apps/*/uninstall", .method = HTTP_POST, .handler = h_app_uninstall_post,  .user_ctx = NULL },
    { .uri = "/api/catalog",          .method = HTTP_GET,  .handler = h_catalog_get,         .user_ctx = NULL },
    { .uri = "/api/catalog/refresh",  .method = HTTP_POST, .handler = h_catalog_refresh_post,.user_ctx = NULL },
    { .uri = "/api/logs",             .method = HTTP_GET,  .handler = h_logs_get,            .user_ctx = NULL },
    { .uri = "/api/launcher/open/*",  .method = HTTP_POST, .handler = h_launcher_open_post,  .user_ctx = NULL },
    { .uri = "/api/info",             .method = HTTP_GET,  .handler = h_info_get,            .user_ctx = NULL },
};

esp_err_t frame_store_api_start(void)
{
    if (!CONFIG_FRAME_HTTP_API_ENABLE) {
        ESP_LOGI(TAG, "device HTTP API disabled in config");
        return ESP_OK;
    }
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.stack_size = API_HTTPD_STACK;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof(k_uris) / sizeof(k_uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, &k_uris[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "route %s not registered: %s", k_uris[i].uri, esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "device HTTP API on port 80");
    return ESP_OK;
}
