#include "rest_api_talk_internal.h"

#if ORB_TALK_WS_ENABLED
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_REST;

esp_err_t talk_live_ws_handler(httpd_req_t *req)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int sockfd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        bool busy = false;
        esp_err_t busy_err = talk_check_busy(&busy);
        if (busy_err != ESP_OK) {
            return ESP_FAIL;
        }
        if (busy) {
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, "talk_busy");
        }
        esp_err_t mark_err = talk_live_mark_open(sockfd);
        if (mark_err != ESP_OK) {
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, "talk_busy");
        }
        ESP_LOGI(TAG, "live ws open fd=%d postfx=fast", sockfd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { 0 };
    uint8_t *payload_buf = NULL;
    esp_err_t recv_err = httpd_ws_recv_frame(req, &frame, 0);
    if (recv_err != ESP_OK) {
        ESP_LOGW(TAG, "live ws recv len failed fd=%d: %s", sockfd, esp_err_to_name(recv_err));
        (void)talk_live_stop_if_owner(sockfd, true);
        return recv_err;
    }

    if (frame.len > 0) {
        if (frame.len > TALK_LIVE_MAX_RX_BYTES) {
            ESP_LOGW(TAG, "live ws frame too large fd=%d len=%u", sockfd, (unsigned)frame.len);
            (void)talk_live_stop_if_owner(sockfd, true);
            return ESP_ERR_INVALID_SIZE;
        }
        payload_buf = (uint8_t *)heap_caps_malloc(frame.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (payload_buf == NULL) {
            payload_buf = (uint8_t *)heap_caps_malloc(frame.len, MALLOC_CAP_8BIT);
        }
        if (payload_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        frame.payload = payload_buf;
        recv_err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (recv_err != ESP_OK) {
            ESP_LOGW(TAG, "live ws recv payload failed fd=%d: %s", sockfd, esp_err_to_name(recv_err));
            (void)talk_live_stop_if_owner(sockfd, true);
            free(payload_buf);
            return recv_err;
        }
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "live ws close fd=%d", sockfd);
        (void)talk_live_stop_if_owner(sockfd, true);
        free(payload_buf);
        return ESP_OK;
    }

    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        if (frame.len >= 4U && memcmp(frame.payload, "stop", 4U) == 0) {
            ESP_LOGI(TAG, "live ws stop requested fd=%d", sockfd);
            (void)talk_live_stop_if_owner(sockfd, true);
        }
        free(payload_buf);
        return ESP_OK;
    }

    if (frame.type != HTTPD_WS_TYPE_BINARY) {
        free(payload_buf);
        return ESP_OK;
    }

    esp_err_t owner_err = talk_live_accept_chunk_owner(sockfd);
    if (owner_err != ESP_OK) {
        ESP_LOGW(TAG, "live ws chunk rejected fd=%d: %s", sockfd, esp_err_to_name(owner_err));
        free(payload_buf);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t chunk_err = talk_live_send_pcm_chunk_bytes(frame.payload, frame.len);
    if (chunk_err != ESP_OK) {
        ESP_LOGW(TAG, "live ws pcm chunk failed fd=%d: %s", sockfd, esp_err_to_name(chunk_err));
        (void)talk_live_stop_if_owner(sockfd, true);
        free(payload_buf);
        return chunk_err;
    }

    free(payload_buf);
    return ESP_OK;
}
#endif
