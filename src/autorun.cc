#include "autorun.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
// 0x530010 autorun_mutex
static HANDLE gInterplayGenericAutorunMutex;
#endif

namespace fallout {

// 0x4139C0 autorun_mutex_create
// Neutered for co-op: host and client must be able to run side by side on
// the same machine (LAN testing). The single-instance guard is disabled.
bool autorunMutexCreate()
{
    return true;
}

// 0x413A00 autorun_mutex_destroy
void autorunMutexClose()
{
}

} // namespace fallout
