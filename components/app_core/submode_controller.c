#include "submode_controller.h"

#include <inttypes.h>
#include "config_manager.h"
#include "control_dispatch.h"
#include "esp_check.h"
#include "esp_log.h"
#include "log_tags.h"
#include "orb_led_scenes.h"
#include "sdkconfig.h"

static const char *TAG = LOG_TAG_MODE_MANAGER;

static uint8_t s_install_scene_step;

static orb_offline_submode_t next_offline_submode(orb_offline_submode_t current)
{
    switch (current) {
    case ORB_OFFLINE_SUBMODE_AURA:
        return ORB_OFFLINE_SUBMODE_LOTTERY;
    case ORB_OFFLINE_SUBMODE_LOTTERY:
        return ORB_OFFLINE_SUBMODE_PROPHECY;
    case ORB_OFFLINE_SUBMODE_PROPHECY:
    default:
        return ORB_OFFLINE_SUBMODE_AURA;
    }
}

static uint32_t lottery_idle_scene(void)
{
    uint32_t scene = (uint32_t)CONFIG_ORB_OFFLINE_LOTTERY_SCENE_IDLE_ID;
    if (scene == ORB_LED_SCENE_ID_FIRE2012) {
        scene = ORB_LED_SCENE_ID_LOTTERY_IDLE;
    }
    return scene;
}

esp_err_t submode_controller_init(void)
{
    s_install_scene_step = 0U;
    return ESP_OK;
}

uint32_t submode_controller_idle_scene_for_mode(orb_mode_t mode)
{
    if (mode == ORB_MODE_OFFLINE_SCRIPTED) {
        orb_runtime_config_t cfg = { 0 };
        if (config_manager_get_snapshot(&cfg) == ESP_OK) {
            switch (cfg.offline_submode) {
            case ORB_OFFLINE_SUBMODE_LOTTERY:
                return lottery_idle_scene();
            case ORB_OFFLINE_SUBMODE_PROPHECY:
                return (uint32_t)CONFIG_ORB_OFFLINE_PROPHECY_SCENE_IDLE_ID;
            case ORB_OFFLINE_SUBMODE_AURA:
            default:
                return ORB_LED_SCENE_ID_FIRE2012;
            }
        }
        return ORB_LED_SCENE_ID_FIRE2012;
    }

    if (mode == ORB_MODE_HYBRID_AI) {
        return ORB_LED_SCENE_ID_HYBRID_IDLE_SLOW_BREATHE;
    }
    if (mode == ORB_MODE_INSTALLATION_SLAVE) {
        return ORB_LED_SCENE_ID_COLOR_WAVE;
    }
    return ORB_LED_SCENE_ID_IDLE_BREATHE;
}

bool submode_controller_is_offline_lottery_active(orb_mode_t mode)
{
    if (mode != ORB_MODE_OFFLINE_SCRIPTED) {
        return false;
    }
    orb_runtime_config_t cfg = { 0 };
    if (config_manager_get_snapshot(&cfg) != ESP_OK) {
        return false;
    }
    return cfg.offline_submode == ORB_OFFLINE_SUBMODE_LOTTERY;
}

esp_err_t submode_controller_handle_request(orb_mode_t mode)
{
    if (mode == ORB_MODE_OFFLINE_SCRIPTED) {
        orb_runtime_config_t cfg = { 0 };
        ESP_RETURN_ON_ERROR(config_manager_get_snapshot(&cfg), TAG, "config snapshot failed");

        orb_offline_submode_t prev = cfg.offline_submode;
        orb_offline_submode_t next = next_offline_submode(prev);
        ESP_RETURN_ON_ERROR(config_manager_set_offline_submode(next), TAG, "set offline submode failed");

        ESP_LOGI(TAG,
                 "offline submode switched: %s -> %s",
                 config_manager_offline_submode_to_str(prev),
                 config_manager_offline_submode_to_str(next));

        (void)control_dispatch_queue_led_touch_overlay_clear();
        (void)control_dispatch_queue_led_scene(submode_controller_idle_scene_for_mode(mode), 0U);
        return ESP_OK;
    }

    if (mode == ORB_MODE_HYBRID_AI) {
        ESP_LOGI(TAG, "hybrid submode action ignored: LED scene is owned by hybrid flow");
        return ESP_OK;
    }

    if (mode == ORB_MODE_INSTALLATION_SLAVE) {
        uint32_t scene = (s_install_scene_step % 2U == 0U) ? ORB_LED_SCENE_ID_COLOR_WAVE : ORB_LED_SCENE_ID_IDLE_BREATHE;
        s_install_scene_step++;
        (void)control_dispatch_queue_led_scene(scene, 0U);
        ESP_LOGI(TAG, "installation submode action: scene=%" PRIu32, scene);
        return ESP_OK;
    }

    return ESP_ERR_INVALID_STATE;
}
