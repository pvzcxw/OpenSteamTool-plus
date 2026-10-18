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

    bool IsSharedLicense(AppId_t appId);
    bool HasValidLicense(AppId_t appId);

}
