#ifndef BSP_BOARD_H
#define BSP_BOARD_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_board_init(void);
const char *bsp_board_name(void);

#ifdef __cplusplus
}
#endif

#endif
