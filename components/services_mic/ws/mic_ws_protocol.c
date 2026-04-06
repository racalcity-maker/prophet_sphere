#include "mic_ws_protocol.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ascii_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static const char *find_json_key(const char *json, const char *key)
{
    if (json == NULL || key == NULL || key[0] == '\0') {
        return NULL;
    }
    char pat[48];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pat)) {
        return NULL;
    }
    return strstr(json, pat);
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return false;
    }
    out[0] = '\0';
    const char *p = find_json_key(json, key);
    if (p == NULL) {
        return false;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t i = 0U;
    while (*p != '\0' && *p != '"' && i < (out_size - 1U)) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0U);
}

static bool json_get_double(const char *json, const char *key, double *out_value)
{
    if (out_value == NULL) {
        return false;
    }
    const char *p = find_json_key(json, key);
    if (p == NULL) {
        return false;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) {
        return false;
    }
    *out_value = v;
    return true;
}

static bool json_get_u32(const char *json, const char *key, uint32_t *out_value)
{
    double v = 0.0;
    if (!json_get_double(json, key, &v)) {
        return false;
    }
    if (v < 0.0) {
        return false;
    }
    if (v > (double)UINT32_MAX) {
        v = (double)UINT32_MAX;
    }
    *out_value = (uint32_t)(v + 0.5);
    return true;
}

static uint16_t confidence_to_permille(double value)
{
    if (value < 0.0) {
        value = 0.0;
    }
    if (value <= 1.0) {
        value *= 1000.0;
    }
    if (value > 1000.0) {
        value = 1000.0;
    }
    return (uint16_t)(value + 0.5);
}

static size_t json_escape_copy(const char *src, char *dst, size_t dst_len)
{
    if (dst == NULL || dst_len == 0U) {
        return 0U;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return 0U;
    }

    size_t j = 0U;
    for (size_t i = 0U; src[i] != '\0'; ++i) {
        char c = src[i];
        if (c == '\\' || c == '"') {
            if (j + 2U >= dst_len) {
                dst[0] = '\0';
                return SIZE_MAX;
            }
            dst[j++] = '\\';
            dst[j++] = c;
        } else if ((unsigned char)c >= 0x20U) {
            if (j + 1U >= dst_len) {
                dst[0] = '\0';
                return SIZE_MAX;
            }
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
    return j;
}

bool mic_ws_protocol_parse_result_message(const char *json,
                                          uint32_t active_capture_id,
                                          uint32_t *out_capture_id,
                                          orb_intent_id_t *out_intent,
                                          uint16_t *out_conf)
{
    if (json == NULL || out_capture_id == NULL || out_intent == NULL || out_conf == NULL) {
        return false;
    }

    char type[24];
    if (json_get_string(json, "type", type, sizeof(type))) {
        if (!ascii_ieq(type, "result") && !ascii_ieq(type, "intent")) {
            return false;
        }
    }

    char intent_str[40];
    if (!json_get_string(json, "intent", intent_str, sizeof(intent_str))) {
        return false;
    }

    uint32_t capture_id = active_capture_id;
    (void)json_get_u32(json, "capture_id", &capture_id);

    double conf_v = 0.0;
    if (!json_get_double(json, "confidence", &conf_v)) {
        if (!json_get_double(json, "confidence_permille", &conf_v) &&
            !json_get_double(json, "conf", &conf_v)) {
            conf_v = 0.0;
        }
    }

    *out_capture_id = capture_id;
    *out_intent = orb_intent_from_string(intent_str);
    *out_conf = confidence_to_permille(conf_v);
    return true;
}

bool mic_ws_protocol_parse_tts_control_message(const char *json, bool *out_done, bool *out_failed)
{
    if (out_done != NULL) {
        *out_done = false;
    }
    if (out_failed != NULL) {
        *out_failed = false;
    }
    if (json == NULL) {
        return false;
    }

    char type[24];
    if (!json_get_string(json, "type", type, sizeof(type))) {
        return false;
    }

    if (ascii_ieq(type, "tts_end") || ascii_ieq(type, "tts_done") || ascii_ieq(type, "end")) {
        if (out_done != NULL) {
            *out_done = true;
        }
        return true;
    }
    if (ascii_ieq(type, "tts_error") || ascii_ieq(type, "error")) {
        if (out_failed != NULL) {
            *out_failed = true;
        }
        return true;
    }
    return false;
}

esp_err_t mic_ws_protocol_build_start_frame(uint32_t capture_id, uint32_t sample_rate_hz, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(out,
                     out_size,
                     "{\"type\":\"start\",\"capture_id\":%" PRIu32 ",\"sample_rate\":%" PRIu32 "}",
                     capture_id,
                     sample_rate_hz);
    if (n <= 0 || n >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t mic_ws_protocol_build_end_frame(uint32_t capture_id, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(out, out_size, "{\"type\":\"end\",\"capture_id\":%" PRIu32 "}", capture_id);
    if (n <= 0 || n >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t mic_ws_protocol_build_tts_request(const char *text,
                                            uint32_t sample_rate_hz,
                                            const char *voice,
                                            char *out,
                                            size_t out_size)
{
    if (text == NULL || text[0] == '\0' || voice == NULL || voice[0] == '\0' || out == NULL || out_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    char escaped[1024];
    size_t escaped_len = json_escape_copy(text, escaped, sizeof(escaped));
    if (escaped_len == SIZE_MAX || escaped_len == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    int n = snprintf(out,
                     out_size,
                     "{\"type\":\"tts\",\"text\":\"%s\",\"sample_rate\":%" PRIu32 ",\"voice\":\"%s\"}",
                     escaped,
                     sample_rate_hz,
                     voice);
    if (n <= 0 || n >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
