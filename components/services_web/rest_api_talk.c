#include "rest_api_modules.h"
#include "rest_api_talk_internal.h"

#include "esp_check.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_REST;

esp_err_t rest_api_register_talk_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(talk_live_init(), TAG, "talk live init failed");

    const httpd_uri_t talk_say = {
        .uri = "/api/talk/say",
        .method = HTTP_POST,
        .handler = talk_say_handler,
        .user_ctx = NULL,
    };
#if ORB_TALK_WS_ENABLED
    const httpd_uri_t talk_live_ws = {
        .uri = TALK_LIVE_WS_PATH,
        .method = HTTP_GET,
        .handler = talk_live_ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };
#endif

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &talk_say), TAG, "register talk say failed");
#if ORB_TALK_WS_ENABLED
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &talk_live_ws), TAG, "register talk ws failed");
#endif
    return ESP_OK;
}
