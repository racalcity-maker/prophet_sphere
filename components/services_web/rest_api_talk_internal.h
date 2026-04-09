#ifndef REST_API_TALK_INTERNAL_H
#define REST_API_TALK_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_media_gateway.h"
#include "config_schema.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "sdkconfig.h"

#define TALK_TEXT_MAX_CHARS 320U
#define TALK_TEXT_ENCODED_MAX_CHARS 2048U
#define TALK_DEFAULT_STREAM_TIMEOUT_MS 90000U
#define TALK_MIN_STREAM_TIMEOUT_MS 1000U
#define TALK_MAX_STREAM_TIMEOUT_MS 180000U
#define TALK_BG_GAIN_SWITCH_FADE_MS 250U
#define TALK_FORM_BODY_MAX_CHARS 2304U
#define TALK_BODY_READ_DEADLINE_MS 2500U

#if CONFIG_HTTPD_WS_SUPPORT
#define ORB_TALK_WS_ENABLED 1
#else
#define ORB_TALK_WS_ENABLED 0
#endif

#if ORB_TALK_WS_ENABLED
#define TALK_LIVE_WS_PATH "/ws/talk"
#define TALK_LIVE_SAMPLE_RATE_HZ 44100U
#define TALK_LIVE_INACTIVITY_TIMEOUT_MS 5000U
#define TALK_LIVE_WATCHDOG_PERIOD_MS 250U
#define TALK_LIVE_MAX_RX_BYTES (AUDIO_PCM_STREAM_CHUNK_MAX_SAMPLES * sizeof(int16_t) * 6U)
#define TALK_LIVE_PREBUFFER_SAMPLES (TALK_LIVE_SAMPLE_RATE_HZ / 2U) /* 0.5s pre-roll before start */
#define TALK_LIVE_RING_CAPACITY_SAMPLES (TALK_LIVE_SAMPLE_RATE_HZ)  /* 1.0s total ring capacity */
#define TALK_LIVE_FEED_TASK_STACK 6144U
#define TALK_LIVE_FEED_TASK_PRIORITY 2U
#define TALK_LIVE_FEED_IDLE_MS 2U
#define TALK_LIVE_POSTFX_DECLICK_THRESHOLD 900
#define TALK_LIVE_POSTFX_DECLICK_RAMP_SAMPLES 24U
#define TALK_LIVE_POSTFX_DC_BETA_Q15 32604
#define TALK_LIVE_POSTFX_LIMITER_THRESHOLD 28000
#define TALK_LIVE_POSTFX_LIMITER_KNEE 2400
#endif

uint32_t talk_request_timeout_ms(void);
esp_err_t talk_start_bg_for_say(const orb_runtime_config_t *cfg);

esp_err_t talk_say_lock(void);
void talk_say_unlock(void);

esp_err_t talk_read_form_body(httpd_req_t *req, char *out, size_t out_len);
esp_err_t talk_get_param(httpd_req_t *req, const char *form_body, const char *key, char *out, size_t out_len);
bool talk_url_decode_inplace(char *text);

esp_err_t talk_check_busy(bool *busy_out);
esp_err_t talk_say_handler(httpd_req_t *req);

esp_err_t talk_live_init(void);
esp_err_t talk_live_snapshot(bool *open_out, bool *stream_out);

#if ORB_TALK_WS_ENABLED
esp_err_t talk_live_mark_open(int sockfd);
esp_err_t talk_live_accept_chunk_owner(int sockfd);
esp_err_t talk_live_stop_if_owner(int sockfd, bool close_socket_state);
esp_err_t talk_live_send_pcm_chunk_bytes(uint8_t *data, size_t len);
esp_err_t talk_live_ws_handler(httpd_req_t *req);
#endif

#endif
