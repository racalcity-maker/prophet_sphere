#include "offline_submode.h"

#include <stddef.h>
#include "config_manager.h"
#include "esp_log.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_MODE_OFFLINE;

static const offline_submode_handler_t *s_active;
static bool s_initialized;

static const offline_submode_handler_t *handlers_at(size_t idx)
{
    switch (idx) {
    case 0:
        return offline_submode_aura_get();
    case 1:
        return offline_submode_lottery_get();
    case 2:
        return offline_submode_prophecy_get();
    default:
        return NULL;
    }
}

static size_t handlers_count(void)
{
    return 3U;
}

static const offline_submode_handler_t *find_handler(orb_offline_submode_t id)
{
    for (size_t i = 0; i < handlers_count(); ++i) {
        const offline_submode_handler_t *handler = handlers_at(i);
        if (handler != NULL && handler->id == id) {
            return handler;
        }
    }
    return offline_submode_aura_get();
}

static const offline_submode_handler_t *resolve_current_handler(void)
{
    orb_offline_submode_t submode = ORB_OFFLINE_SUBMODE_AURA;
    if (config_manager_get_offline_submode(&submode) != ESP_OK) {
        return offline_submode_aura_get();
    }
    return find_handler(submode);
}

static esp_err_t call_enter(const offline_submode_handler_t *handler)
{
    if (handler == NULL || handler->enter == NULL) {
        return ESP_OK;
    }
    return handler->enter();
}

static esp_err_t call_exit(const offline_submode_handler_t *handler)
{
    if (handler == NULL || handler->exit == NULL) {
        return ESP_OK;
    }
    return handler->exit();
}

esp_err_t offline_submode_router_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    for (size_t i = 0; i < handlers_count(); ++i) {
        const offline_submode_handler_t *handler = handlers_at(i);
        if (handler == NULL) {
            continue;
        }
        if (handler->init != NULL) {
            esp_err_t err = handler->init();
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    s_initialized = true;
    s_active = NULL;
    ESP_LOGI(TAG, "offline submode router initialized");
    return ESP_OK;
}

esp_err_t offline_submode_router_enter(void)
{
    esp_err_t err = offline_submode_router_init();
    if (err != ESP_OK) {
        return err;
    }

    const offline_submode_handler_t *next = resolve_current_handler();
    err = call_enter(next);
    if (err != ESP_OK) {
        return err;
    }
    s_active = next;
    if (s_active != NULL) {
        ESP_LOGI(TAG, "offline submode active: %s", s_active->name);
    }
    return ESP_OK;
}

esp_err_t offline_submode_router_exit(void)
{
    esp_err_t err = call_exit(s_active);
    if (err != ESP_OK) {
        return err;
    }
    s_active = NULL;
    return ESP_OK;
}

esp_err_t offline_submode_router_handle_event(const app_mode_event_t *event, app_mode_action_t *action)
{
    if (event == NULL || action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = offline_submode_router_init();
    if (err != ESP_OK) {
        return err;
    }

    const offline_submode_handler_t *next = resolve_current_handler();
    if (next != s_active) {
        err = call_exit(s_active);
        if (err != ESP_OK) {
            return err;
        }
        err = call_enter(next);
        if (err != ESP_OK) {
            return err;
        }
        s_active = next;
        if (s_active != NULL) {
            ESP_LOGI(TAG, "offline submode switched: %s", s_active->name);
        }
    }

    if (s_active == NULL || s_active->handle_event == NULL) {
        *action = (app_mode_action_t){ 0 };
        return ESP_OK;
    }

    return s_active->handle_event(event, action);
}
