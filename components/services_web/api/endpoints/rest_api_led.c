#include "rest_api_modules.h"

#include <stdio.h>
#include "app_api.h"
#include "app_media_gateway.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "log_tags.h"
#include "rest_api_common.h"

static const char *TAG = LOG_TAG_REST;

static uint32_t request_timeout_ms(void)
{
    return app_media_gateway_queue_timeout_ms();
}

static esp_err_t led_scene_handler(httpd_req_t *req)
{
    char scene_text[16];
    char duration_text[16];

    if (rest_api_query_value(req, "scene", scene_text, sizeof(scene_text)) != ESP_OK) {
        return rest_api_send_error_json(req, "400 Bad Request", "missing_scene");
    }

    uint32_t scene_id = 0;
    if (!rest_api_parse_u32(scene_text, &scene_id)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_scene");
    }

    uint32_t duration_ms = 0;
    if (rest_api_query_value(req, "duration_ms", duration_text, sizeof(duration_text)) == ESP_OK) {
        if (!rest_api_parse_u32(duration_text, &duration_ms)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_duration_ms");
        }
    }

    led_command_t cmd = { 0 };
    cmd.id = LED_CMD_PLAY_SCENE;
    cmd.payload.play_scene.scene_id = scene_id;
    cmd.payload.play_scene.duration_ms = duration_ms;
    esp_err_t err = app_media_gateway_send_led_command(&cmd, request_timeout_ms());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led scene failed: %s", esp_err_to_name(err));
        return rest_api_send_error_json(req, "500 Internal Server Error", "led_scene_failed");
    }

    char json[192];
    (void)snprintf(json,
                   sizeof(json),
                   "{\"ok\":true,\"scene\":%lu,\"duration_ms\":%lu}",
                   (unsigned long)scene_id,
                   (unsigned long)duration_ms);
    return rest_api_send_json(req, "200 OK", json);
}

static esp_err_t led_brightness_handler(httpd_req_t *req)
{
    char value_text[16];
    if (rest_api_query_value(req, "value", value_text, sizeof(value_text)) != ESP_OK) {
        return rest_api_send_error_json(req, "400 Bad Request", "missing_value");
    }

    uint32_t value = 0;
    if (!rest_api_parse_u32(value_text, &value) || value > 255U) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_value");
    }

    led_command_t cmd = { 0 };
    cmd.id = LED_CMD_SET_BRIGHTNESS;
    cmd.payload.set_brightness.brightness = (uint8_t)value;
    esp_err_t err = app_media_gateway_send_led_command(&cmd, request_timeout_ms());
    if (err != ESP_OK) {
        return rest_api_send_error_json(req, "500 Internal Server Error", "led_brightness_failed");
    }
    (void)app_api_set_led_brightness((uint8_t)value, false);

    char json[80];
    (void)snprintf(json, sizeof(json), "{\"ok\":true,\"brightness\":%lu}", (unsigned long)value);
    return rest_api_send_json(req, "200 OK", json);
}

static bool parse_u8_query(httpd_req_t *req, const char *key, uint8_t *value, bool *present)
{
    char text[16];
    if (present != NULL) {
        *present = false;
    }
    if (rest_api_query_value(req, key, text, sizeof(text)) != ESP_OK) {
        return true;
    }
    uint32_t parsed = 0U;
    if (!rest_api_parse_u32(text, &parsed) || parsed > 255U) {
        return false;
    }
    if (value != NULL) {
        *value = (uint8_t)parsed;
    }
    if (present != NULL) {
        *present = true;
    }
    return true;
}

static esp_err_t led_effect_handler(httpd_req_t *req)
{
    uint32_t scene_id = 0U;
    uint32_t duration_ms = 0U;
    bool has_scene = false;

    char scene_text[16];
    if (rest_api_query_value(req, "scene", scene_text, sizeof(scene_text)) == ESP_OK) {
        if (!rest_api_parse_u32(scene_text, &scene_id)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_scene");
        }
        has_scene = true;
    }

    char duration_text[16];
    if (rest_api_query_value(req, "duration_ms", duration_text, sizeof(duration_text)) == ESP_OK) {
        if (!rest_api_parse_u32(duration_text, &duration_ms)) {
            return rest_api_send_error_json(req, "400 Bad Request", "invalid_duration_ms");
        }
    }

    uint8_t speed = 0U;
    uint8_t intensity = 0U;
    uint8_t scale = 0U;
    uint8_t palette_mode = 0U;
    uint8_t c1_r = 0U;
    uint8_t c1_g = 0U;
    uint8_t c1_b = 0U;
    uint8_t c2_r = 0U;
    uint8_t c2_g = 0U;
    uint8_t c2_b = 0U;
    uint8_t c3_r = 0U;
    uint8_t c3_g = 0U;
    uint8_t c3_b = 0U;
    bool has_speed = false;
    bool has_intensity = false;
    bool has_scale = false;
    bool has_palette_mode = false;
    bool has_c1_r = false;
    bool has_c1_g = false;
    bool has_c1_b = false;
    bool has_c2_r = false;
    bool has_c2_g = false;
    bool has_c2_b = false;
    bool has_c3_r = false;
    bool has_c3_g = false;
    bool has_c3_b = false;

    if (!parse_u8_query(req, "speed", &speed, &has_speed)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_speed");
    }
    if (!parse_u8_query(req, "intensity", &intensity, &has_intensity)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_intensity");
    }
    if (!parse_u8_query(req, "scale", &scale, &has_scale)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_scale");
    }
    if (!parse_u8_query(req, "palette_mode", &palette_mode, &has_palette_mode)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_palette_mode");
    }
    if (!parse_u8_query(req, "palette_c1_r", &c1_r, &has_c1_r)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_palette_c1_r");
    }
    if (!parse_u8_query(req, "palette_c1_g", &c1_g, &has_c1_g)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_palette_c1_g");
    }
    if (!parse_u8_query(req, "palette_c1_b", &c1_b, &has_c1_b)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_palette_c1_b");
    }
    if (!parse_u8_query(req, "palette_c2_r", &c2_r, &has_c2_r)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_palette_c2_r");
    }
    if (!parse_u8_query(req, "palette_c2_g", &c2_g, &has_c2_g)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_palette_c2_g");
    }
    if (!parse_u8_query(req, "palette_c2_b", &c2_b, &has_c2_b)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_palette_c2_b");
    }
    if (!parse_u8_query(req, "palette_c3_r", &c3_r, &has_c3_r)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_palette_c3_r");
    }
    if (!parse_u8_query(req, "palette_c3_g", &c3_g, &has_c3_g)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_palette_c3_g");
    }
    if (!parse_u8_query(req, "palette_c3_b", &c3_b, &has_c3_b)) {
        return rest_api_send_error_json(req, "400 Bad Request", "invalid_palette_c3_b");
    }

    bool has_any_effect_param = has_speed || has_intensity || has_scale;
    bool has_any_palette_param = has_palette_mode || has_c1_r || has_c1_g || has_c1_b || has_c2_r || has_c2_g ||
                                 has_c2_b || has_c3_r || has_c3_g || has_c3_b;
    if (has_any_effect_param && !(has_speed && has_intensity && has_scale)) {
        return rest_api_send_error_json(req, "400 Bad Request", "effect_params_partial");
    }
    if (has_any_palette_param &&
        !(has_palette_mode && has_c1_r && has_c1_g && has_c1_b && has_c2_r && has_c2_g && has_c2_b && has_c3_r &&
          has_c3_g && has_c3_b)) {
        return rest_api_send_error_json(req, "400 Bad Request", "effect_palette_partial");
    }
    if (!has_scene && !has_any_effect_param && !has_any_palette_param) {
        return rest_api_send_error_json(req, "400 Bad Request", "missing_scene_or_effect_params");
    }

    if (has_any_effect_param) {
        led_command_t cmd = { 0 };
        cmd.id = LED_CMD_SET_EFFECT_PARAMS;
        cmd.payload.set_effect_params.speed = speed;
        cmd.payload.set_effect_params.intensity = intensity;
        cmd.payload.set_effect_params.scale = scale;
        esp_err_t err = app_media_gateway_send_led_command(&cmd, request_timeout_ms());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "led effect params failed: %s", esp_err_to_name(err));
            return rest_api_send_error_json(req, "500 Internal Server Error", "led_effect_params_failed");
        }
    }

    if (has_any_palette_param) {
        led_command_t cmd = { 0 };
        cmd.id = LED_CMD_SET_EFFECT_PALETTE;
        cmd.payload.set_effect_palette.mode = palette_mode;
        cmd.payload.set_effect_palette.c1_r = c1_r;
        cmd.payload.set_effect_palette.c1_g = c1_g;
        cmd.payload.set_effect_palette.c1_b = c1_b;
        cmd.payload.set_effect_palette.c2_r = c2_r;
        cmd.payload.set_effect_palette.c2_g = c2_g;
        cmd.payload.set_effect_palette.c2_b = c2_b;
        cmd.payload.set_effect_palette.c3_r = c3_r;
        cmd.payload.set_effect_palette.c3_g = c3_g;
        cmd.payload.set_effect_palette.c3_b = c3_b;
        esp_err_t err = app_media_gateway_send_led_command(&cmd, request_timeout_ms());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "led effect palette failed: %s", esp_err_to_name(err));
            return rest_api_send_error_json(req, "500 Internal Server Error", "led_effect_palette_failed");
        }
    }

    if (has_scene) {
        led_command_t cmd = { 0 };
        cmd.id = LED_CMD_PLAY_SCENE;
        cmd.payload.play_scene.scene_id = scene_id;
        cmd.payload.play_scene.duration_ms = duration_ms;
        esp_err_t err = app_media_gateway_send_led_command(&cmd, request_timeout_ms());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "led effect scene failed: %s", esp_err_to_name(err));
            return rest_api_send_error_json(req, "500 Internal Server Error", "led_effect_scene_failed");
        }
    }

    char json[256];
    (void)snprintf(json,
                   sizeof(json),
                   "{\"ok\":true,\"scene\":%lu,\"duration_ms\":%lu,\"speed\":%u,\"intensity\":%u,\"scale\":%u,"
                   "\"palette_mode\":%u}",
                   (unsigned long)scene_id,
                   (unsigned long)duration_ms,
                   (unsigned)(has_speed ? speed : 0U),
                   (unsigned)(has_intensity ? intensity : 0U),
                   (unsigned)(has_scale ? scale : 0U),
                   (unsigned)(has_palette_mode ? palette_mode : 0U));
    return rest_api_send_json(req, "200 OK", json);
}

esp_err_t rest_api_register_led_handlers(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t scene = {
        .uri = "/api/led/scene",
        .method = HTTP_POST,
        .handler = led_scene_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t brightness = {
        .uri = "/api/led/brightness",
        .method = HTTP_POST,
        .handler = led_brightness_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t effect = {
        .uri = "/api/led/effect",
        .method = HTTP_POST,
        .handler = led_effect_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &scene), TAG, "register led scene failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &brightness), TAG, "register led brightness failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &effect), TAG, "register led effect failed");
    return ESP_OK;
}
