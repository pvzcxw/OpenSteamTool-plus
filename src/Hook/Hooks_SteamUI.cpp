#include "Hooks_SteamUI.h"
#include "Hooks_Package.h"
#include "HookManager.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "steam_messages.pb.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include <atomic>
#include "Utils/HookSupport/VehCommon.h"
#include <mutex>
#include <unordered_set>
#include <vector>

namespace
{
    RESOLVE_FUNC(RepeatedFieldUint32_Add, void, void* field, const uint32* value);

    CAPTURE_THIS_FUNC(GetAppByID, CSteamApp*, g_pController,void* pThis, AppId_t appId, bool bCreate);
    CAPTURE_THIS_FUNC(MarkAppChange,void*,g_pAppChangeSource,void* pThis,AppId_t appId, EAppChangeFlags changeFlags);

    std::atomic<uint32_t> g_selfAccountId{0};

    HOOK_FUNC(FillInAppOverview, void *, void *pThis, void *pAppOverview, CSteamApp *pApp)
    {
        if (pApp && LuaConfig::HasDepot(pApp->nAppID, false))
        {
            if (pApp->SteamID.GetAccountID() == 0 &&
                Hooks_Package::GetFamilySharedOwner(pApp->nAppID) == 0)
            {
                uint32_t selfAccount = g_selfAccountId.load(std::memory_order_relaxed);
                if (selfAccount == 0)
                {
                    std::wstring universe;
                    if (OSTPlatform::SteamCredentialStore::GetActiveUser(selfAccount, universe) ==
                        OSTPlatform::SteamCredentialStore::Status::Ok)
                        g_selfAccountId.store(selfAccount, std::memory_order_relaxed);
                }

                if (selfAccount != 0)
                {
                    pApp->SteamID.Set(selfAccount, k_EUniversePublic, k_EAccountTypeIndividual);
                    LOG_STEAMUI_DEBUG("FillInAppOverview: appId={} owner set to self ({})",
                                      pApp->nAppID, selfAccount);
                }
            }

            uint32_t t = LuaConfig::GetPurchaseTime(pApp->nAppID);
            if (t)
            {
                pApp->PurchasedTime = t;
                LOG_STEAMUI_TRACE("FillInAppOverview: set PurchasedTime={} for appId={}",
                                  pApp->PurchasedTime, pApp->nAppID);
            }
        }
        return oFillInAppOverview(pThis, pAppOverview, pApp);
    }

    // Apps to drop from the library: queued off-thread, marked on the UI thread.
    std::mutex g_removalMutex;
    std::vector<AppId_t> g_pendingRemovals;
    std::unordered_set<AppId_t> g_removedAppIds;

    // A full rebuild never lists removed_appid for apps still in the map
    // so re-assert our set after the snapshot is built.
    HOOK_FUNC(BuildCompleteAppOverviewChange, void, void *pController,
              CAppOverview_Change *pChange, void *optionalCallbackSlot)
    {
        oBuildCompleteAppOverviewChange(pController, pChange, optionalCallbackSlot);
        std::lock_guard<std::mutex> lock(g_removalMutex);
        if (pChange && !g_removedAppIds.empty() && oRepeatedFieldUint32_Add)
        {
            auto* field = pChange->mutable_removed_appid();
            for (AppId_t appId : g_removedAppIds){
                oRepeatedFieldUint32_Add(field, &appId);
            }
            LOG_STEAMUI_DEBUG("BuildCompleteAppOverviewChange: appended {} removed_appid entries",
                              g_removedAppIds.size());
        }
    }


    // Clearing ownership makes ShouldShowAppInLibrary() false (delta drops it,
    // the full snapshot skips it); MarkAppChange triggers the flush.
    HOOK_FUNC(CSteamUIAppControllerRunFrame, void *, void *pController)
    {
        if (CAPTURE_READY(GetAppByID) && CAPTURE_READY(MarkAppChange))
        {
            std::vector<AppId_t> draining;
            {
                std::lock_guard<std::mutex> lock(g_removalMutex);
                draining.swap(g_pendingRemovals);
            }
            for (AppId_t appId : draining)
            {
                if (LuaConfig::IsOwned(appId))
                {
                    LOG_STEAMUI_DEBUG("RunFrame: appId {} is owned again, skipping removal", appId);
                    continue;
                }
                if (CSteamApp *pApp = oGetAppByID(g_pController, appId, false))
                {
                    // Only remove from the library if it's not already uninstalled
                    pApp->OwnershipFlags = k_EAppOwnershipFlags_None;
                    if(pApp->AppStateFlags == k_EAppStateUninstalled){
                        std::lock_guard<std::mutex> lock(g_removalMutex);
                        g_removedAppIds.insert(appId);
                    }
                }
                
                oMarkAppChange(g_pAppChangeSource, appId, EAppChangeFlags::AppInfoOrConfig);
            }
        }
        return oCSteamUIAppControllerRunFrame(pController);
    }
}

namespace Hooks_SteamUI
{
    void Install()
    {
        ARM_CAPTURE_U(GetAppByID);
        ARM_CAPTURE_U(MarkAppChange);

        RESOLVE_U(RepeatedFieldUint32_Add);

        HOOK_BEGIN();
        INSTALL_HOOK_U(FillInAppOverview);
        INSTALL_HOOK_U(BuildCompleteAppOverviewChange);
        INSTALL_HOOK_U(CSteamUIAppControllerRunFrame);
        HOOK_END();
    }

    void Uninstall()
    {
        g_selfAccountId.store(0, std::memory_order_relaxed);
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(FillInAppOverview);
        UNINSTALL_HOOK(BuildCompleteAppOverviewChange);
        UNINSTALL_HOOK(CSteamUIAppControllerRunFrame);
        UNHOOK_END();
    }

    void QueueRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        g_pendingRemovals.push_back(appId);
    }

    void CancelRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        std::erase(g_pendingRemovals, appId);
        g_removedAppIds.erase(appId);
    }
}
