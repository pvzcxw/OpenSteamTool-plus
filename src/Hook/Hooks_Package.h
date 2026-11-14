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

    // Returns the remembered family lender account ID for an AppId, or 0 if none.
    AccountID_t GetFamilySharedOwner(AppId_t appId);

}
