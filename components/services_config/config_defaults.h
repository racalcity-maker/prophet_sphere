#ifndef CONFIG_DEFAULTS_H
#define CONFIG_DEFAULTS_H

#include "config_schema.h"

/*
 * Internal defaults provider.
 * Reads boot-time defaults from menuconfig-backed CONFIG_* values.
 */
orb_runtime_config_t config_defaults_load(void);

#endif
