#ifndef OFFLINE_SUBMODE_H
#define OFFLINE_SUBMODE_H

#include "app_mode.h"
#include "config_schema.h"
#include "esp_err.h"

typedef struct {
    orb_offline_submode_t id;
    const char *name;
    esp_err_t (*init)(void);
    esp_err_t (*enter)(void);
    esp_err_t (*exit)(void);
    esp_err_t (*handle_event)(const app_mode_event_t *event, app_mode_action_t *action);
} offline_submode_handler_t;

const offline_submode_handler_t *offline_submode_aura_get(void);
const offline_submode_handler_t *offline_submode_lottery_get(void);
const offline_submode_handler_t *offline_submode_prophecy_get(void);

esp_err_t offline_submode_router_init(void);
esp_err_t offline_submode_router_enter(void);
esp_err_t offline_submode_router_exit(void);
esp_err_t offline_submode_router_handle_event(const app_mode_event_t *event, app_mode_action_t *action);

#endif
