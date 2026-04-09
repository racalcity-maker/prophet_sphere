#include "web_portal.h"

#include <stddef.h>
#include "app_api.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_WEB;

typedef enum {
    PAGE_MODE_ANY = 0,
    PAGE_MODE_OFFLINE,
    PAGE_MODE_HYBRID,
    PAGE_MODE_INSTALLATION,
} page_mode_t;

typedef struct {
    const char *uri;
    const char *content_type;
    const uint8_t *start;
    const uint8_t *end;
    page_mode_t page_mode;
} embedded_page_t;

extern const uint8_t _binary_offline_html_start[] asm("_binary_offline_html_start");
extern const uint8_t _binary_offline_html_end[] asm("_binary_offline_html_end");
extern const uint8_t _binary_hybrid_html_start[] asm("_binary_hybrid_html_start");
extern const uint8_t _binary_hybrid_html_end[] asm("_binary_hybrid_html_end");
extern const uint8_t _binary_installation_html_start[] asm("_binary_installation_html_start");
extern const uint8_t _binary_installation_html_end[] asm("_binary_installation_html_end");
extern const uint8_t _binary_app_css_start[] asm("_binary_app_css_start");
extern const uint8_t _binary_app_css_end[] asm("_binary_app_css_end");
extern const uint8_t _binary_app_js_start[] asm("_binary_app_js_start");
extern const uint8_t _binary_app_js_end[] asm("_binary_app_js_end");
extern const uint8_t _binary_dom_http_js_start[] asm("_binary_dom_http_js_start");
extern const uint8_t _binary_dom_http_js_end[] asm("_binary_dom_http_js_end");
extern const uint8_t _binary_hybrid_effects_js_start[] asm("_binary_hybrid_effects_js_start");
extern const uint8_t _binary_hybrid_effects_js_end[] asm("_binary_hybrid_effects_js_end");
extern const uint8_t _binary_talk_js_start[] asm("_binary_talk_js_start");
extern const uint8_t _binary_talk_js_end[] asm("_binary_talk_js_end");
extern const uint8_t _binary_server_tts_js_start[] asm("_binary_server_tts_js_start");
extern const uint8_t _binary_server_tts_js_end[] asm("_binary_server_tts_js_end");
extern const uint8_t _binary_page_offline_js_start[] asm("_binary_page_offline_js_start");
extern const uint8_t _binary_page_offline_js_end[] asm("_binary_page_offline_js_end");
extern const uint8_t _binary_network_setup_page_js_start[] asm("_binary_network_setup_page_js_start");
extern const uint8_t _binary_network_setup_page_js_end[] asm("_binary_network_setup_page_js_end");
extern const uint8_t _binary_page_mode_generic_js_start[] asm("_binary_page_mode_generic_js_start");
extern const uint8_t _binary_page_mode_generic_js_end[] asm("_binary_page_mode_generic_js_end");

static esp_err_t send_embedded(httpd_req_t *req, const embedded_page_t *page)
{
    if (req == NULL || page == NULL || page->start == NULL || page->end == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = (size_t)(page->end - page->start);
    if (len > 0U && page->start[len - 1U] == '\0') {
        len--;
    }
    httpd_resp_set_type(req, page->content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)page->start, len);
}

static esp_err_t root_redirect_handler(httpd_req_t *req)
{
    const char *home = app_api_mode_home_uri(app_api_get_current_mode());
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", home);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t mode_redirect_handler(httpd_req_t *req)
{
    const char *home = app_api_mode_home_uri(app_api_get_current_mode());
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", home);
    return httpd_resp_send(req, NULL, 0);
}

static bool page_allowed_for_mode(page_mode_t page_mode, orb_mode_t mode)
{
    /* During early boot mode can be NONE; allow portal page open to avoid redirect loops. */
    if (mode == ORB_MODE_NONE) {
        return true;
    }
    if (page_mode == PAGE_MODE_ANY) {
        return true;
    }
    if (page_mode == PAGE_MODE_OFFLINE && mode == ORB_MODE_OFFLINE_SCRIPTED) {
        return true;
    }
    if (page_mode == PAGE_MODE_HYBRID && mode == ORB_MODE_HYBRID_AI) {
        return true;
    }
    if (page_mode == PAGE_MODE_INSTALLATION && mode == ORB_MODE_INSTALLATION_SLAVE) {
        return true;
    }
    return false;
}

static esp_err_t embedded_handler(httpd_req_t *req)
{
    const embedded_page_t *page = (const embedded_page_t *)req->user_ctx;
    if (page == NULL) {
        return ESP_FAIL;
    }

    orb_mode_t mode = app_api_get_current_mode();
    if (!page_allowed_for_mode(page->page_mode, mode)) {
        const char *home = app_api_mode_home_uri(mode);
        ESP_LOGI(TAG, "redirect %s -> %s (mode=%s)", req->uri, home, app_api_mode_to_str(mode));
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", home);
        return httpd_resp_send(req, NULL, 0);
    }

    return send_embedded(req, page);
}

esp_err_t web_portal_register_http_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    static const embedded_page_t pages[] = {
        { .uri = "/offline",
          .content_type = "text/html; charset=utf-8",
          .start = _binary_offline_html_start,
          .end = _binary_offline_html_end,
          .page_mode = PAGE_MODE_OFFLINE },
        { .uri = "/hybrid",
          .content_type = "text/html; charset=utf-8",
          .start = _binary_hybrid_html_start,
          .end = _binary_hybrid_html_end,
          .page_mode = PAGE_MODE_HYBRID },
        { .uri = "/installation",
          .content_type = "text/html; charset=utf-8",
          .start = _binary_installation_html_start,
          .end = _binary_installation_html_end,
          .page_mode = PAGE_MODE_INSTALLATION },
        { .uri = "/assets/css/app.css",
          .content_type = "text/css; charset=utf-8",
          .start = _binary_app_css_start,
          .end = _binary_app_css_end,
          .page_mode = PAGE_MODE_ANY },
        { .uri = "/assets/js/app.js",
          .content_type = "application/javascript; charset=utf-8",
          .start = _binary_app_js_start,
          .end = _binary_app_js_end,
          .page_mode = PAGE_MODE_ANY },
        { .uri = "/assets/js/core/dom_http.js",
          .content_type = "application/javascript; charset=utf-8",
          .start = _binary_dom_http_js_start,
          .end = _binary_dom_http_js_end,
          .page_mode = PAGE_MODE_ANY },
        { .uri = "/assets/js/features/hybrid_effects.js",
          .content_type = "application/javascript; charset=utf-8",
          .start = _binary_hybrid_effects_js_start,
          .end = _binary_hybrid_effects_js_end,
          .page_mode = PAGE_MODE_ANY },
        { .uri = "/assets/js/features/talk.js",
          .content_type = "application/javascript; charset=utf-8",
          .start = _binary_talk_js_start,
          .end = _binary_talk_js_end,
          .page_mode = PAGE_MODE_ANY },
        { .uri = "/assets/js/features/server_tts.js",
          .content_type = "application/javascript; charset=utf-8",
          .start = _binary_server_tts_js_start,
          .end = _binary_server_tts_js_end,
          .page_mode = PAGE_MODE_ANY },
        { .uri = "/assets/js/pages/page_offline.js",
          .content_type = "application/javascript; charset=utf-8",
          .start = _binary_page_offline_js_start,
          .end = _binary_page_offline_js_end,
          .page_mode = PAGE_MODE_ANY },
        { .uri = "/assets/js/pages/network_setup_page.js",
          .content_type = "application/javascript; charset=utf-8",
          .start = _binary_network_setup_page_js_start,
          .end = _binary_network_setup_page_js_end,
          .page_mode = PAGE_MODE_ANY },
        { .uri = "/assets/js/pages/page_mode_generic.js",
          .content_type = "application/javascript; charset=utf-8",
          .start = _binary_page_mode_generic_js_start,
          .end = _binary_page_mode_generic_js_end,
          .page_mode = PAGE_MODE_ANY },
    };

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_redirect_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), TAG, "register / failed");

    const httpd_uri_t mode = {
        .uri = "/mode",
        .method = HTTP_GET,
        .handler = mode_redirect_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &mode), TAG, "register /mode failed");

    for (size_t i = 0; i < (sizeof(pages) / sizeof(pages[0])); ++i) {
        const httpd_uri_t uri = {
            .uri = pages[i].uri,
            .method = HTTP_GET,
            .handler = embedded_handler,
            .user_ctx = (void *)&pages[i],
        };
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uri), TAG, "register %s failed", pages[i].uri);
    }

    ESP_LOGI(TAG, "portal handlers registered");
    return ESP_OK;
}
