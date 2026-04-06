#include "app_fsm.h"

#include "esp_log.h"
#include "mode_manager.h"
#include "session_controller.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_APP_FSM;
static app_fsm_state_t s_state = APP_FSM_STATE_UNINITIALIZED;

esp_err_t app_fsm_init(void)
{
    s_state = APP_FSM_STATE_BOOTING;
    (void)session_controller_init();
    ESP_LOGI(TAG, "FSM initialized");
    return ESP_OK;
}

esp_err_t app_fsm_handle_event(const app_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (event->id) {
    case APP_EVENT_MODE_SWITCH_REQUEST: {
        orb_mode_t target = event->payload.mode.to_mode;
        s_state = APP_FSM_STATE_MODE_TRANSITION;
        esp_err_t err = mode_manager_perform_switch(target);
        if (err != ESP_OK) {
            s_state = APP_FSM_STATE_ERROR;
            ESP_LOGW(TAG, "mode switch to %s failed: %s", mode_manager_mode_to_str(target), esp_err_to_name(err));
            return err;
        }
        s_state = APP_FSM_STATE_RUNNING;
        ESP_LOGI(TAG, "mode changed to %s", mode_manager_mode_to_str(mode_manager_get_current_mode()));
        return ESP_OK;
    }
    case APP_EVENT_SUBMODE_BUTTON_REQUEST: {
        esp_err_t err = mode_manager_handle_submode_request();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "submode request failed: %s", esp_err_to_name(err));
            return err;
        }
        return ESP_OK;
    }
    case APP_EVENT_NETWORK_RECONFIGURE_REQUEST: {
        esp_err_t err = mode_manager_reconfigure_runtime_for_current_mode();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "network reconfigure request failed: %s", esp_err_to_name(err));
            return err;
        }
        return ESP_OK;
    }
    case APP_EVENT_NETWORK_DOWN:
    case APP_EVENT_AUDIO_ERROR:
    case APP_EVENT_MIC_ERROR:
    case APP_EVENT_MIC_REMOTE_PLAN_ERROR:
    case APP_EVENT_MIC_TTS_ERROR:
    case APP_EVENT_AI_ERROR:
        s_state = APP_FSM_STATE_DEGRADED;
        return ESP_OK;
    case APP_EVENT_NETWORK_UP:
        if (s_state == APP_FSM_STATE_DEGRADED) {
            s_state = APP_FSM_STATE_RUNNING;
        }
        return ESP_OK;
    default:
        if (s_state == APP_FSM_STATE_BOOTING) {
            s_state = APP_FSM_STATE_RUNNING;
        }
        return mode_manager_dispatch_event(event);
    }
}

app_fsm_state_t app_fsm_get_state(void)
{
    return s_state;
}

const char *app_fsm_state_to_str(app_fsm_state_t state)
{
    switch (state) {
    case APP_FSM_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APP_FSM_STATE_BOOTING:
        return "BOOTING";
    case APP_FSM_STATE_RUNNING:
        return "RUNNING";
    case APP_FSM_STATE_MODE_TRANSITION:
        return "MODE_TRANSITION";
    case APP_FSM_STATE_DEGRADED:
        return "DEGRADED";
    case APP_FSM_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
