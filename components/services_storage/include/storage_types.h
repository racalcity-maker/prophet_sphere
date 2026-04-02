#ifndef STORAGE_TYPES_H
#define STORAGE_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool mounted;
    uint64_t total_bytes;
    uint64_t free_bytes;
    char mount_point[32];
} storage_status_t;

#ifdef __cplusplus
}
#endif

#endif
