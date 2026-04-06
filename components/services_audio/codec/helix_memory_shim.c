#include "third_party/helix/utils/helix_memory.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

void *helix_malloc(int size)
{
    if (size <= 0) {
        size = 1;
    }

    void *ptr = heap_caps_calloc(1, (size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr != NULL) {
        return ptr;
    }

    ptr = calloc(1, (size_t)size);
    return ptr;
}

void helix_free(void *ptr)
{
    free(ptr);
}

