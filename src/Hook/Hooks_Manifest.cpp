#include "Hooks_Manifest.h"
#include "HookMacros.h"
#include "dllmain.h"
#include <format>

// ═══════════════════════════════════════════════════════════════════
//  Manifest override hooks:
//    BuildDepotDependency — patches depot entries' gid/size directly
//      in the output vector (replaces the old KV-tree approach).
// ═══════════════════════════════════════════════════════════════════
namespace {

    template <typename T>
    bool IsWellFormedVector(const CUtlVector<T>* vec) {
        return !vec || (vec->m_Size <= vec->m_Memory.m_nAllocationCount &&
                        (vec->m_Memory.m_nAllocationCount == 0 ||
                         vec->m_Memory.m_pMemory != nullptr));
    }

    std::string DepotEntryDebug(const DepotEntry& e) {
        return std::format("DepotId={} AppId={} Gid={} Size={} Dlc={} Lcs={} Carry={} Shared={}",
            e.DepotId, e.AppId, e.ManifestGid, e.ManifestSize, e.DlcAppId,
            (int)e.LcsRequired, (int)e.bNotNewTarget, (int)e.SharedInstall);
    }

    HOOK_FUNC(BuildDepotDependency, bool, void* pUserAppMgr, AppId_t AppId,
              void* pUserConfig, CUtlVector<DepotEntry>* pDepotInfo,
              CUtlVector<DepotEntry>* pSharedDepotInfo, void* pSteamApp,
              uint32* pBuildId, bool* pbBetaFallback)
    {
        bool result = oBuildDepotDependency(pUserAppMgr, AppId, pUserConfig,
            pDepotInfo, pSharedDepotInfo, pSteamApp, pBuildId, pbBetaFallback);

        // Do not walk Steam-owned vector memory unless its pointer/count/capacity
        // agree. A transient appinfo refresh can leave the output vectors empty;
        // malformed metadata must take the original failure path rather than
        // crash while logging or patching it.
        if (!IsWellFormedVector(pDepotInfo) || !IsWellFormedVector(pSharedDepotInfo)) {
            LOG_MANIFEST_WARN("BuildDepotDependency: app {} returned an invalid depot vector; preserving previous config",
                              AppId);
            return false;
        }

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

        if (!result) return result;

        // During a PICS refresh Steam can briefly drop the app's own depot list
        // before publishing the replacement appinfo. Letting an empty result
        // through makes Steam persist a zero-depot install as fully installed.
        // The shared list is intentionally excluded: it contains depots owned
        // by other apps and cannot establish that this app's own list is valid.
        if (AppId != 0 && (!pDepotInfo || pDepotInfo->m_Size == 0) &&
            LuaConfig::HasDepot(AppId, false)) {
            LOG_MANIFEST_WARN("BuildDepotDependency: app {} returned 0 own depots during appinfo refresh; preserving previous config",
                              AppId);
            return false;
        }

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
