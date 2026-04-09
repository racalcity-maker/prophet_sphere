#include "rest_api_talk_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "log_tags.h"
#include "rest_api_common.h"

static const char *TAG = LOG_TAG_REST;

static esp_err_t talk_say_send_pcm_start(uint32_t timeout_ms)
{
    audio_command_t cmd = { .id = AUDIO_CMD_PCM_STREAM_START };
    return app_tasking_send_audio_command(&cmd, timeout_ms);
}

static esp_err_t talk_say_send_pcm_stop(uint32_t timeout_ms)
{
    audio_command_t cmd = { .id = AUDIO_CMD_PCM_STREAM_STOP };
    return app_tasking_send_audio_command(&cmd, timeout_ms);
}

static esp_err_t talk_say_enqueue_tts_play(const char *text,
                                           uint32_t stream_timeout_ms,
                                           uint32_t bg_fade_out_ms,
                                           uint32_t queue_timeout_ms)
{
#if !CONFIG_ORB_ENABLE_MIC
    (void)text;
    (void)stream_timeout_ms;
    (void)bg_fade_out_ms;
    (void)queue_timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(text) >= MIC_TTS_TEXT_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    mic_command_t cmd = { 0 };
    cmd.id = MIC_CMD_TTS_PLAY_TEXT;
    (void)snprintf(cmd.payload.tts_play.text, sizeof(cmd.payload.tts_play.text), "%s", text);
    cmd.payload.tts_play.timeout_ms = stream_timeout_ms;
    cmd.payload.tts_play.bg_fade_out_ms = bg_fade_out_ms;
    return app_tasking_send_mic_command(&cmd, queue_timeout_ms);
#endif
}

esp_err_t talk_say_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    char *form_body = NULL;
    char *text_encoded = NULL;
    if (talk_say_lock() != ESP_OK) {
        return rest_api_send_error_json(req, "409 Conflict", "talk_busy");
    }

    form_body = (char *)heap_caps_calloc(TALK_FORM_BODY_MAX_CHARS + 1U, sizeof(char), MALLOC_CAP_8BIT);
    text_encoded = (char *)heap_caps_calloc(TALK_TEXT_ENCODED_MAX_CHARS + 1U, sizeof(char), MALLOC_CAP_8BIT);
    if (form_body == NULL || text_encoded == NULL) {
        ret = rest_api_send_error_json(req, "500 Internal Server Error", "out_of_memory");
        goto cleanup;
    }

    esp_err_t body_err = talk_read_form_body(req, form_body, TALK_FORM_BODY_MAX_CHARS + 1U);
    if (body_err == ESP_ERR_INVALID_SIZE) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "request_too_large");
        goto cleanup;
    }
    if (body_err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "talk say body read timeout content_len=%d", req ? req->content_len : -1);
        ret = rest_api_send_error_json(req, "408 Request Timeout", "request_body_timeout");
        goto cleanup;
    }
    if (body_err != ESP_OK && body_err != ESP_ERR_NOT_FOUND) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "invalid_request_body");
        goto cleanup;
    }

    esp_err_t text_err = talk_get_param(req, form_body, "text", text_encoded, TALK_TEXT_ENCODED_MAX_CHARS + 1U);
    if (text_err == ESP_ERR_INVALID_SIZE) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "text_too_long");
        goto cleanup;
    }
    if (text_err != ESP_OK || text_encoded[0] == '\0') {
        ret = rest_api_send_error_json(req, "400 Bad Request", "missing_text");
        goto cleanup;
    }
    if (strchr(text_encoded, '%') != NULL || strchr(text_encoded, '+') != NULL) {
        if (!talk_url_decode_inplace(text_encoded)) {
            ret = rest_api_send_error_json(req, "400 Bad Request", "invalid_text_encoding");
            goto cleanup;
        }
    }
    if (text_encoded[0] == '\0') {
        ret = rest_api_send_error_json(req, "400 Bad Request", "missing_text");
        goto cleanup;
    }
    if (strlen(text_encoded) > TALK_TEXT_MAX_CHARS) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "text_too_long");
        goto cleanup;
    }
    if (strlen(text_encoded) >= sizeof(((mic_command_t *)0)->payload.tts_play.text)) {
        ret = rest_api_send_error_json(req, "400 Bad Request", "text_too_long");
        goto cleanup;
    }

    char text[TALK_TEXT_MAX_CHARS + 1U];
    (void)strncpy(text, text_encoded, sizeof(text) - 1U);
    text[sizeof(text) - 1U] = '\0';

    uint32_t stream_timeout_ms = TALK_DEFAULT_STREAM_TIMEOUT_MS;
    bool with_bg = true;
    uint32_t bg_fade_out_ms = 0U;
    char timeout_text[16];
    char with_bg_text[16];
    if (talk_get_param(req, form_body, "timeout_ms", timeout_text, sizeof(timeout_text)) == ESP_OK) {
        if (!rest_api_parse_u32(timeout_text, &stream_timeout_ms) ||
            stream_timeout_ms < TALK_MIN_STREAM_TIMEOUT_MS ||
            stream_timeout_ms > TALK_MAX_STREAM_TIMEOUT_MS) {
            ret = rest_api_send_error_json(req, "400 Bad Request", "invalid_timeout_ms");
            goto cleanup;
        }
    }
    if (talk_get_param(req, form_body, "with_bg", with_bg_text, sizeof(with_bg_text)) == ESP_OK) {
        if (!rest_api_parse_bool_text(with_bg_text, &with_bg)) {
            ret = rest_api_send_error_json(req, "400 Bad Request", "invalid_with_bg");
            goto cleanup;
        }
    }

#if !CONFIG_ORB_ENABLE_MIC
        ret = rest_api_send_error_json(req, "503 Service Unavailable", "mic_service_disabled");
        goto cleanup;
#endif

    bool busy = false;
    esp_err_t busy_err = talk_check_busy(&busy);
    if (busy_err != ESP_OK) {
        ESP_LOGW(TAG, "talk busy check failed: %s", esp_err_to_name(busy_err));
        ret = rest_api_send_error_json(req, "500 Internal Server Error", "talk_busy_check_failed");
        goto cleanup;
    }
    if (busy) {
        ret = rest_api_send_error_json(req, "409 Conflict", "talk_busy");
        goto cleanup;
    }

    if (with_bg) {
        orb_runtime_config_t cfg = { 0 };
        if (config_manager_get_snapshot(&cfg) != ESP_OK) {
            ret = rest_api_send_error_json(req, "500 Internal Server Error", "config_read_failed");
            goto cleanup;
        }
        esp_err_t bg_err = talk_start_bg_for_say(&cfg);
        if (bg_err != ESP_OK) {
            ESP_LOGW(TAG, "talk say bg start failed: %s", esp_err_to_name(bg_err));
            ret = rest_api_send_error_json(req, "500 Internal Server Error", "talk_bg_start_failed");
            goto cleanup;
        }
        bg_fade_out_ms = cfg.prophecy_bg_fade_out_ms;
    }

    uint32_t queue_timeout_ms = talk_request_timeout_ms();
    esp_err_t pcm_err = talk_say_send_pcm_start(queue_timeout_ms);
    if (pcm_err != ESP_OK) {
        ESP_LOGW(TAG, "talk say pcm stream start failed: %s", esp_err_to_name(pcm_err));
        ret = rest_api_send_error_json(req, "500 Internal Server Error", "talk_pcm_start_failed");
        goto cleanup;
    }

    esp_err_t err = talk_say_enqueue_tts_play(text, stream_timeout_ms, bg_fade_out_ms, queue_timeout_ms);
    if (err != ESP_OK) {
        esp_err_t rollback_err = talk_say_send_pcm_stop(queue_timeout_ms);
        if (rollback_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "talk say failed (%s), and pcm rollback failed: %s",
                     esp_err_to_name(err),
                     esp_err_to_name(rollback_err));
        } else {
            ESP_LOGW(TAG, "talk say failed (%s), rolled back pcm stream", esp_err_to_name(err));
        }
        if (err == ESP_ERR_INVALID_SIZE) {
            ret = rest_api_send_error_json(req, "400 Bad Request", "text_too_long");
            goto cleanup;
        }
        ret = rest_api_send_error_json(req, "500 Internal Server Error", "talk_tts_failed");
        goto cleanup;
    }
    ESP_LOGI(TAG,
             "talk say accepted chars=%u timeout_ms=%lu with_bg=%u bg_fade_out_ms=%" PRIu32,
             (unsigned)strlen(text),
             (unsigned long)stream_timeout_ms,
             with_bg ? 1U : 0U,
             bg_fade_out_ms);

    char json[136];
    (void)snprintf(json,
                   sizeof(json),
                   "{\"ok\":true,\"chars\":%u,\"timeout_ms\":%lu,\"with_bg\":%s,\"bg_fade_out_ms\":%" PRIu32 "}",
                   (unsigned)strlen(text),
                   (unsigned long)stream_timeout_ms,
                   with_bg ? "true" : "false",
                   bg_fade_out_ms);
    ret = rest_api_send_json(req, "200 OK", json);

cleanup:
    if (text_encoded != NULL) {
        heap_caps_free(text_encoded);
    }
    if (form_body != NULL) {
        heap_caps_free(form_body);
    }
    talk_say_unlock();
    return ret;
}
