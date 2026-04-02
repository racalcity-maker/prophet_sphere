#ifndef ERROR_CODES_H
#define ERROR_CODES_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ORB_ERR_BASE 0x7100
#define ORB_ERR_NOT_INITIALIZED (ORB_ERR_BASE + 1)
#define ORB_ERR_ALREADY_STARTED (ORB_ERR_BASE + 2)
#define ORB_ERR_MODE_NOT_REGISTERED (ORB_ERR_BASE + 3)

#ifdef __cplusplus
}
#endif

#endif
