#ifndef CONFIG_DEFAULTS_H
#define CONFIG_DEFAULTS_H

#include "config_schema.h"

/*
 * Internal defaults provider.
 * Reads boot-time defaults from menuconfig-backed CONFIG_* values.
 */
void config_defaults_load(orb_runtime_config_t *cfg);

#endif
