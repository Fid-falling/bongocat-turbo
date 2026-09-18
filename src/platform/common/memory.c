#include "bongo_cat/memory.h"

#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

void bongo_cat_platform_trim_memory(void) {
#ifdef _WIN32
    /* Release allocator slack, then let Windows keep only actively touched
       pages resident. GPU resources and private commit remain intact. */
    _heapmin();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
#elif defined(__APPLE__)
    malloc_zone_pressure_relief(NULL, 0);
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
}
