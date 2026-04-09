#include "rest_api_talk_internal.h"

#include <ctype.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "log_tags.h"
#include "rest_api_common.h"

static SemaphoreHandle_t s_talk_say_lock;

uint32_t talk_request_timeout_ms(void)
{
    return (uint32_t)CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS;
}

static uint16_t talk_bg_gain_for_tts(uint16_t configured_gain_permille)
{
    return (configured_gain_permille > 180U) ? 180U : configured_gain_permille;
}

esp_err_t talk_start_bg_for_say(const orb_runtime_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_command_t cmd = { 0 };
    cmd.id = AUDIO_CMD_BG_SET_GAIN;
    /* Match hybrid "answer" transition profile for spoken response. */
    cmd.payload.bg_set_gain.fade_ms = TALK_BG_GAIN_SWITCH_FADE_MS;
    cmd.payload.bg_set_gain.gain_permille = talk_bg_gain_for_tts(cfg->prophecy_bg_gain_permille);
    return app_tasking_send_audio_command(&cmd, talk_request_timeout_ms());
}

esp_err_t talk_say_lock(void)
{
    if (s_talk_say_lock == NULL) {
        s_talk_say_lock = xSemaphoreCreateMutex();
        if (s_talk_say_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    TickType_t ticks = pdMS_TO_TICKS(CONFIG_ORB_QUEUE_SEND_TIMEOUT_MS);
    if (ticks == 0) {
        ticks = 1;
    }
    if (xSemaphoreTake(s_talk_say_lock, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void talk_say_unlock(void)
{
    if (s_talk_say_lock != NULL) {
        xSemaphoreGive(s_talk_say_lock);
    }
}

esp_err_t talk_read_form_body(httpd_req_t *req, char *out, size_t out_len)
{
    if (req == NULL || out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (req->content_len <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((size_t)req->content_len >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    int remaining = req->content_len;
    int total = 0;
    int64_t deadline_us = esp_timer_get_time() + ((int64_t)TALK_BODY_READ_DEADLINE_MS * 1000LL);
    while (remaining > 0) {
        int read_len = httpd_req_recv(req, out + total, remaining);
        if (read_len <= 0) {
            if (read_len == HTTPD_SOCK_ERR_TIMEOUT) {
                if (esp_timer_get_time() >= deadline_us) {
                    return ESP_ERR_TIMEOUT;
                }
                continue;
            }
            return ESP_FAIL;
        }
        total += read_len;
        remaining -= read_len;
    }
    out[total] = '\0';
    return ESP_OK;
}

esp_err_t talk_get_param(httpd_req_t *req, const char *form_body, const char *key, char *out, size_t out_len)
{
    if (req == NULL || key == NULL || out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t qerr = rest_api_query_value(req, key, out, out_len);
    if (qerr == ESP_OK) {
        return ESP_OK;
    }
    if (form_body != NULL && form_body[0] != '\0') {
        esp_err_t ferr = httpd_query_key_value(form_body, key, out, out_len);
        if (ferr == ESP_OK) {
            return ESP_OK;
        }
        if (ferr == ESP_ERR_HTTPD_RESULT_TRUNC) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static int talk_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') {
        return 10 + (int)(c - 'a');
    }
    return -1;
}

bool talk_url_decode_inplace(char *text)
{
    if (text == NULL) {
        return false;
    }
    char *src = text;
    char *dst = text;
    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }
        if (*src == '%') {
            if (src[1] == '\0' || src[2] == '\0') {
                return false;
            }
            int hi = talk_hex_nibble(src[1]);
            int lo = talk_hex_nibble(src[2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
    return true;
}

esp_err_t talk_check_busy(bool *busy_out)
{
    if (busy_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *busy_out = false;

    session_info_t session = { 0 };
    esp_err_t sess_err = session_controller_get_info(&session);
    if (sess_err != ESP_OK) {
        return sess_err;
    }
    if (session.active) {
        *busy_out = true;
        return ESP_OK;
    }

    bool live_open = false;
    bool live_stream = false;
    esp_err_t live_err = talk_live_snapshot(&live_open, &live_stream);
    if (live_err == ESP_OK && (live_open || live_stream)) {
        *busy_out = true;
    }

    return ESP_OK;
}
