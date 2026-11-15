#include "Hooks_Manifest.h"
#include "Hooks_Package.h"
#include "Hooks_Misc.h"
#include "HookMacros.h"
#include "dllmain.h"
#include <algorithm>
#include <format>
#include <string>

// ═══════════════════════════════════════════════════════════════════
//  Manifest override hooks:
//    BuildDepotDependency — patches depot entries' gid/size directly
//      in the output vector (replaces the old KV-tree approach).
// ═══════════════════════════════════════════════════════════════════
namespace {

    std::string DepotEntryDebug(const DepotEntry& e) {
        return std::format("DepotId={} AppId={} Gid={} Size={} Dlc={} Lcs={} Carry={} Shared={}",
            e.DepotId, e.AppId, e.ManifestGid, e.ManifestSize, e.DlcAppId,
            (int)e.LcsRequired, (int)e.bNotNewTarget, (int)e.SharedInstall);
    }

    std::string DepotIdList(const CUtlVector<DepotEntry>* entries) {
        if (!entries) return "<null>";
        if ((entries->m_Size != 0 && !entries->m_Memory.m_pMemory) ||
            entries->m_Size > entries->m_Memory.m_nAllocationCount)
            return std::format("<invalid size={} allocation={} memory=0x{:X}>",
                entries->m_Size, entries->m_Memory.m_nAllocationCount,
                reinterpret_cast<uintptr_t>(entries->m_Memory.m_pMemory));

        std::string ids = "[";
        const uint32 count = (std::min)(entries->m_Size, 32u);
        for (uint32 i = 0; i < count; ++i) {
            if (i) ids += ',';
            ids += std::to_string(entries->m_Memory.m_pMemory[i].DepotId);
        }
        if (entries->m_Size > count)
            ids += std::format(",...(+{})", entries->m_Size - count);
        ids += ']';
        return ids;
    }

    HOOK_FUNC(BuildDepotDependency, bool, void* pUserAppMgr, AppId_t AppId,
              void* pUserConfig, CUtlVector<DepotEntry>* pDepotInfo,
              CUtlVector<DepotEntry>* pSharedDepotInfo, void* pSteamApp,
              uint32* pBuildId, bool* pbBetaFallback)
    {
        bool result = oBuildDepotDependency(pUserAppMgr, AppId, pUserConfig,
            pDepotInfo, pSharedDepotInfo, pSteamApp, pBuildId, pbBetaFallback);

        LOG_MANIFEST_TRACE("BuildDepotDependency: AppId={} pUserConfig=0x{:X} result={} pSteamApp=0x{:X} pBuildId={} pbBetaFallback={}",
            AppId, (uintptr_t)pUserConfig, result, (uintptr_t)pSteamApp,
            pBuildId ? *pBuildId : 0, pbBetaFallback ? *pbBetaFallback : false);
        if (pDepotInfo) {
            LOG_MANIFEST_TRACE("pDepotInfo->nCount={}", pDepotInfo->m_Size);
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  [{}] {}", i, DepotEntryDebug(pDepotInfo->m_Memory.m_pMemory[i]));
            }
        }
        if (pSharedDepotInfo) {
            LOG_MANIFEST_TRACE("pSharedDepotInfo->nCount={}", pSharedDepotInfo->m_Size);
            for (uint32 i = 0; i < pSharedDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  shared[{}] {}", i, DepotEntryDebug(pSharedDepotInfo->m_Memory.m_pMemory[i]));
            }
        }

        const uint64_t outputDepotCount =
            static_cast<uint64_t>(pDepotInfo ? pDepotInfo->m_Size : 0) +
            static_cast<uint64_t>(pSharedDepotInfo ? pSharedDepotInfo->m_Size : 0);
        const bool configuredApp = LuaConfig::HasDepot(AppId, false);
        const bool appInfoNameAvailable = Hooks_Misc::IsAppInfoId(AppId);
        if (outputDepotCount == 0 && configuredApp) {
            // Startup license indexing may finish before package 0 is filled.
            // Request one retry per app; Hooks_Package consumes it from the
            // next CheckAppOwnership call on Steam's own client thread.
            Hooks_Package::RequestLicenseRebuild(AppId);
            LOG_MANIFEST_WARN("BuildDepotDependency: zero output for configured app {} (original_result={} appinfo_common_name={} own_count={} shared_count={} build_id={} beta_fallback={} pUserConfig=0x{:X} pSteamApp=0x{:X}); queued a license-index retry",
                AppId, result, appInfoNameAvailable,
                pDepotInfo ? pDepotInfo->m_Size : 0,
                pSharedDepotInfo ? pSharedDepotInfo->m_Size : 0,
                pBuildId ? *pBuildId : 0, pbBetaFallback ? *pbBetaFallback : false,
                reinterpret_cast<uintptr_t>(pUserConfig), reinterpret_cast<uintptr_t>(pSteamApp));
            LOG_MANIFEST_WARN("BuildDepotDependency: empty-result details for app {} own_depot_ids={} shared_depot_ids={}",
                AppId, DepotIdList(pDepotInfo), DepotIdList(pSharedDepotInfo));
        }

        // Steam can transiently clear an app's cached depot config while
        // appinfo is being refreshed. The original builder may still return
        // true with an empty own-depot vector; returning that success lets the
        // caller persist an empty config and can complete an install with 0B.
        // Shared depots belong to other apps and must not mask an empty own
        // depot list. Fail this pass so Steam retains the previous config and
        // retries after the pending license/appinfo refresh.
        if (result && AppId != 0 && (!pDepotInfo || pDepotInfo->m_Size == 0) &&
            configuredApp) {
            LOG_MANIFEST_WARN("BuildDepotDependency: configured app {} returned success with no own depots (shared_count={} appinfo_common_name={} build_id={} beta_fallback={} own_depot_ids={} shared_depot_ids={}); preserving the previous depot config",
                AppId, pSharedDepotInfo ? pSharedDepotInfo->m_Size : 0,
                appInfoNameAvailable, pBuildId ? *pBuildId : 0,
                pbBetaFallback ? *pbBetaFallback : false,
                DepotIdList(pDepotInfo), DepotIdList(pSharedDepotInfo));
            return false;
        }

        if (!result && configuredApp) {
            LOG_MANIFEST_WARN("BuildDepotDependency: original function failed for configured app {} (own_count={} shared_count={} appinfo_common_name={} build_id={} beta_fallback={} own_depot_ids={} shared_depot_ids={})",
                AppId, pDepotInfo ? pDepotInfo->m_Size : 0,
                pSharedDepotInfo ? pSharedDepotInfo->m_Size : 0,
                appInfoNameAvailable, pBuildId ? *pBuildId : 0,
                pbBetaFallback ? *pbBetaFallback : false,
                DepotIdList(pDepotInfo), DepotIdList(pSharedDepotInfo));
        }

        if (!result) return result;

        const auto& overrides = LuaConfig::GetManifestOverrides();
        if (overrides.empty()) return result;

        if (pDepotInfo && pDepotInfo->m_Size) {
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                DepotEntry& e = pDepotInfo->m_Memory.m_pMemory[i];
                auto it = overrides.find(e.DepotId);
                if (it != overrides.end()) {
                    // if size=0 in the override, keep the original size(affects download display but not the actual download)
                    uint64_t newSize = it->second.size ? it->second.size : e.ManifestSize;
                    LOG_MANIFEST_INFO("BuildDepotDependency: patching depot {} gid={}->{} size={}->{}",
                        e.DepotId, e.ManifestGid, it->second.gid,
                        e.ManifestSize, newSize);
                    e.ManifestGid  = it->second.gid;
                    e.ManifestSize = newSize;
                }
            }
        }
        return result;
    }

} // anonymous namespace

namespace Hooks_Manifest {

    void Install() {
        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildDepotDependency);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildDepotDependency);
        UNHOOK_END();
    }
}
