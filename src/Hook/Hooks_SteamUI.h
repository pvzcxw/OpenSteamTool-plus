#pragma once

#include "dllmain.h"

// Hooks targeting steamui.dll:

namespace Hooks_SteamUI {
    // Install the built-in SteamUI module-loader hook before loading remote
    // pattern metadata; this hook is independent of PatternLoader.
    bool InstallModuleLoader() noexcept;

    void Install();
    void Uninstall();

    // Queues an appId for removal from the library UI
    void QueueRemoval(AppId_t appId);
    // Cancels a queued removal when the app is added again before the UI drains it.
    void CancelRemoval(AppId_t appId);
}
