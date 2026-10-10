#include "bongo_cat/memory.h"

#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

/* 三个平台的实现其实是同一件事: 把已经还给分配器、但还没还给操作系统的内存退回
   去。放在平台层是因为这些都是 OS 特有的原语(堆收缩 / 工作集裁剪 / malloc_trim),
   core 层没有对应的跨平台抽象。 */
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
