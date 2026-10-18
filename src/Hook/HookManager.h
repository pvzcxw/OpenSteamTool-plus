#ifndef HOOKMANAGER_H
#define HOOKMANAGER_H

#include "dllmain.h"

namespace SteamUI {
    // Installs the loader chokepoint using the built-in SteamUI signature.
    // This must run before PatternLoader performs any remote I/O.
    bool CoreLoaderHook();
    void CoreHook();
    void CoreUnhook();
}

namespace SteamClient {
    void CoreHook();
    void CoreUnhook();
}


#endif // HOOKMANAGER_H
