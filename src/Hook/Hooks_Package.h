#pragma once

#include "dllmain.h"

namespace Hooks_Package
{
    // LoadPackage + CheckAppOwnership — patches the package store so that
    // user-supplied depots appear owned and accessible.
    void Install();
    void Uninstall();

    // Mark package 0 as changed and trigger CClientAppManager_ProcessPendingLicenseUpdates.
    void NotifyLicenseChanged();

    // Queue one app-specific index rebuild. The next CheckAppOwnership call
    // consumes it on Steam's client thread.
    void RequestLicenseRebuild(AppId_t appId);

}
