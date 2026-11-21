#include "Hooks_Package.h"
#include "HookMacros.h"
#include "Hooks_SteamUI.h"
#include "Hooks_Misc.h"
#include "dllmain.h"
#include "Utils/HookSupport/VehCommon.h"
#include "OSTPlatform/include/Memory.h"
#include "Utils/SteamMetadata/PatternLoader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>

namespace {
    // This Steam function is CUtlVector::GrowVector: besides allocating it
    // increases m_Size. Its low-byte return value is not a success indicator.
    RESOLVE_FUNC(CUtlMemoryGrow,               void, CUtlVector<AppId_t>*, int);
    RESOLVE_FUNC(MarkLicenseAsChanged,         int64, void*, uint32, bool);
    RESOLVE_FUNC(ProcessPendingLicenseUpdates, bool,  void*);

    CAPTURE_THIS_FUNC(GetPackageInfo, PackageInfo*, g_pCPackageInfo, void* pThis,
                      uint32 packageId, uint64 accessToken);

    void* g_pCUser = nullptr;
    PackageInfo* g_pInjectedPackageInfo = nullptr;
    bool g_licenseInitialized = false;
    bool g_licenseRefreshPending = false;
    std::mutex g_rebuildMutex;
    std::unordered_map<AppId_t, std::chrono::steady_clock::time_point> g_lastRebuildRequest;
    std::unordered_set<AppId_t> g_pendingRebuilds;
    std::chrono::steady_clock::time_point g_lastLicenseProcessAttempt{};
    std::atomic<uint32_t> g_depotGateConfiguredLogCount{0};
    std::atomic<uint32_t> g_depotGatePassThroughLogCount{0};
    std::atomic<uint32_t> g_ownershipDiagnosticCount{0};
    thread_local bool g_processingLicenseUpdate = false;

    constexpr PackageId_t kInjectedPackageId = 0;
    constexpr uint64_t kInjectedPkgAccessToken = 10660652434190618804ull;

    template <typename T>
    std::string FormatIdVector(const CUtlVector<T>& vec, size_t maxItems = 48)
    {
        if ((vec.m_Size != 0 && !vec.m_Memory.m_pMemory) ||
            vec.m_Size > vec.m_Memory.m_nAllocationCount)
            return std::format("<invalid size={} allocation={}>",
                               vec.m_Size, vec.m_Memory.m_nAllocationCount);

        std::string ids = "[";
        const uint32 count = static_cast<uint32>((std::min)(static_cast<size_t>(vec.m_Size), maxItems));
        for (uint32 i = 0; i < count; ++i)
        {
            if (i) ids += ',';
            ids += std::to_string(vec.m_Memory.m_pMemory[i]);
        }
        if (vec.m_Size > count)
            ids += std::format(",...(+{})", vec.m_Size - count);
        ids += ']';
        return ids;
    }

    std::string PackageVectorsDebug(const PackageInfo* pkg)
    {
        if (!pkg) return "<package-null>";
        return std::format("apps(size={},alloc={},ids={}) depots(size={},alloc={},ids={})",
                           pkg->AppIdVec.m_Size, pkg->AppIdVec.m_Memory.m_nAllocationCount,
                           FormatIdVector(pkg->AppIdVec),
                           pkg->DepotIdVec.m_Size, pkg->DepotIdVec.m_Memory.m_nAllocationCount,
                           FormatIdVector(pkg->DepotIdVec));
    }

    template <typename T>
    bool VectorContainsId(const CUtlVector<T>& vec, T id)
    {
        if ((vec.m_Size != 0 && !vec.m_Memory.m_pMemory) ||
            vec.m_Size > vec.m_Memory.m_nAllocationCount)
            return false;
        for (uint32 i = 0; i < vec.m_Size; ++i)
            if (vec.m_Memory.m_pMemory[i] == id)
                return true;
        return false;
    }

    std::string PackageIdMembershipDebug(const PackageInfo* pkg, AppId_t id)
    {
        if (!pkg) return "<package-null>";
        return std::format("id={} in_apps={} in_depots={}", id,
                           VectorContainsId(pkg->AppIdVec, id),
                           VectorContainsId(pkg->DepotIdVec, id));
    }

    void* ResolveDepotLicenseGate()
    {
#if defined(_M_X64)
        const auto module = client_hModule.load(std::memory_order_acquire);
        if (!module)
        {
            LOG_PACKAGE_WARN("DepotLicenseGate: steamclient module is not loaded");
            return nullptr;
        }

        const auto image = OSTPlatform::Memory::GetModuleImage(module);
        if (!image || !image->base)
            return nullptr;

        if (image->size < sizeof(IMAGE_DOS_HEADER))
            return nullptr;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image->base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
            return nullptr;
        const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
        if (ntOffset > image->size || sizeof(IMAGE_NT_HEADERS64) > image->size - ntOffset)
            return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image->base + ntOffset);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt->OptionalHeader.SizeOfImage > image->size)
        {
            LOG_PACKAGE_WARN("DepotLicenseGate image identity/header mismatch; refusing stale RVA");
            return nullptr;
        }

        // Resolve using the SHA-specific pattern TOML loaded for this DLL.
        // steam-monitor emits both the RVA and a unique signature; the loader
        // selects the record matching this steamclient build's SHA-256.
        auto* candidate = static_cast<uint8_t*>(
            PatternLoader::FindPattern(module, "DepotLicenseGate"));
        if (!candidate)
        {
            LOG_PACKAGE_WARN("DepotLicenseGate has no usable entry in the steamclient pattern TOML");
            return nullptr;
        }

        const uintptr_t base = reinterpret_cast<uintptr_t>(image->base);
        const uintptr_t address = reinterpret_cast<uintptr_t>(candidate);
        const uintptr_t imageSize = nt->OptionalHeader.SizeOfImage;
        if (imageSize > (std::numeric_limits<uintptr_t>::max)() - base ||
            address < base || address - base >= imageSize)
        {
            LOG_PACKAGE_WARN("DepotLicenseGate TOML result 0x{:X} is outside the steamclient image",
                             address);
            return nullptr;
        }
        const uintptr_t rva = address - base;
        const uintptr_t end = base + imageSize;

        DWORD64 unwindImageBase = 0;
        PRUNTIME_FUNCTION runtimeFunction = RtlLookupFunctionEntry(
            static_cast<DWORD64>(address), &unwindImageBase, nullptr);
        if (!runtimeFunction || unwindImageBase != static_cast<DWORD64>(base))
        {
            LOG_PACKAGE_WARN("DepotLicenseGate candidate 0x{:X} is not covered by this image's .pdata",
                             address);
            return nullptr;
        }

        if (runtimeFunction->BeginAddress >= imageSize ||
            runtimeFunction->EndAddress <= runtimeFunction->BeginAddress ||
            runtimeFunction->EndAddress > imageSize)
        {
            LOG_PACKAGE_WARN("DepotLicenseGate .pdata extent is outside the steamclient image");
            return nullptr;
        }

        const uintptr_t functionBegin = base + runtimeFunction->BeginAddress;
        const uintptr_t functionEnd = base + runtimeFunction->EndAddress;
        if (functionBegin < base || functionEnd <= functionBegin || functionEnd > end ||
            address != functionBegin)
        {
            LOG_PACKAGE_WARN("DepotLicenseGate TOML candidate 0x{:X} is not a .pdata function entry [0x{:X},0x{:X})",
                             address, functionBegin, functionEnd);
            return nullptr;
        }

        MEMORY_BASIC_INFORMATION mbi{};
        constexpr DWORD kExecutableProtection = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                                 PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!VirtualQuery(candidate, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & kExecutableProtection) == 0)
        {
            LOG_PACKAGE_WARN("DepotLicenseGate candidate 0x{:X} is not committed executable memory",
                             address);
            return nullptr;
        }

        LOG_PACKAGE_INFO("DepotLicenseGate resolved at {} (RVA 0x{:X}, .pdata [0x{:X},0x{:X}))",
                         address, rva, functionBegin - base, functionEnd - base);
        return candidate;
#else
        return nullptr;
#endif
    }

    bool MarkLicenseAsChangedAndProcessUpdates()
    {
        if (!g_pCUser || !oMarkLicenseAsChanged || !oProcessPendingLicenseUpdates)
        {
            LOG_PACKAGE_WARN("MarkLicenseAsChangedAndProcessUpdates: dependencies not ready, skipping");
            return false;
        }
        if (g_processingLicenseUpdate)
            return true;

        const auto now = std::chrono::steady_clock::now();
        if (g_lastLicenseProcessAttempt.time_since_epoch().count() != 0 &&
            now - g_lastLicenseProcessAttempt < std::chrono::seconds(1))
            return false;
        g_lastLicenseProcessAttempt = now;

        const int64 markResult = oMarkLicenseAsChanged(g_pCUser, kInjectedPackageId, true);
        g_processingLicenseUpdate = true;
        const bool processed = oProcessPendingLicenseUpdates(g_pCUser);
        g_processingLicenseUpdate = false;
        if (!processed)
        {
            // False means Steam kept the pending app/package lists rather
            // than swapping the index; retry on a later client-thread pass.
            LOG_PACKAGE_WARN("MarkLicenseAsChangedAndProcessUpdates: index swap deferred (mark_result={} package_vectors={})",
                             markResult, PackageVectorsDebug(g_pInjectedPackageInfo));
            return false;
        }
        LOG_PACKAGE_INFO("MarkLicenseAsChangedAndProcessUpdates: index rebuilt (package={} mark_result={} package_vectors={})",
                         kInjectedPackageId, markResult,
                         PackageVectorsDebug(g_pInjectedPackageInfo));
        return true;
    }

    void TryProcessPendingLicenseRefresh()
    {
        if (!g_licenseRefreshPending)
            return;
        if (MarkLicenseAsChangedAndProcessUpdates())
            g_licenseRefreshPending = false;
    }

    template <typename T>
    bool CUtlMemoryGrowWrap(CUtlVector<T>* pVec, uint32 additional)
    {
        if (!pVec || !oCUtlMemoryGrow ||
            additional > static_cast<uint32>((std::numeric_limits<int>::max)()) ||
            pVec->m_Size > pVec->m_Memory.m_nAllocationCount ||
            (pVec->m_Size != 0 && !pVec->m_Memory.m_pMemory))
        {
            LOG_PACKAGE_WARN("CUtlMemoryGrowWrap: invalid vector, grow function, or element count");
            return false;
        }

        const uint64_t required = static_cast<uint64_t>(pVec->m_Size) + additional;
        if (required > (std::numeric_limits<uint32>::max)() ||
            required > static_cast<uint64_t>((std::numeric_limits<int>::max)()))
        {
            LOG_PACKAGE_WARN("CUtlMemoryGrowWrap: requested capacity {} overflows supported range", required);
            return false;
        }
        if (required <= pVec->m_Memory.m_nAllocationCount)
            return true;

        // CUtlMemory is the first member of CUtlVector, preserving the engine
        // ABI while keeping both PackageInfo vectors on this checked path.
        auto* abiVector = reinterpret_cast<CUtlVector<AppId_t>*>(pVec);
        const uint32 oldSize = pVec->m_Size;
        oCUtlMemoryGrow(abiVector, static_cast<int>(additional));
        const bool ok = pVec->m_Size == required && pVec->m_Memory.m_pMemory &&
                        pVec->m_Memory.m_nAllocationCount >= required;
        // GrowVector updates logical size too. This wrapper only ensures
        // capacity; AppendUniqueIds writes into the reserved tail itself.
        pVec->m_Size = oldSize;
        if (!ok)
            LOG_PACKAGE_WARN("CUtlMemoryGrowWrap: engine grow failed (required={}, allocated={})",
                             required, pVec->m_Memory.m_nAllocationCount);
        return ok;
    }

    template <typename T>
    bool AppendUniqueIds(CUtlVector<T>* pVec, const std::vector<T>& ids,
                         bool* pChanged = nullptr)
    {
        if (pChanged) *pChanged = false;
        if (!pVec || (pVec->m_Size != 0 && !pVec->m_Memory.m_pMemory) ||
            pVec->m_Size > pVec->m_Memory.m_nAllocationCount)
            return false;

        std::unordered_set<T> present;
        present.reserve(static_cast<size_t>(pVec->m_Size) + ids.size());
        bool changed = false;
        for (uint32 i = 0; i < pVec->m_Size;)
        {
            if (!present.insert(pVec->m_Memory.m_pMemory[i]).second)
            {
                pVec->FastRemove(i);
                changed = true;
                continue;
            }
            ++i;
        }

        std::vector<T> missing;
        missing.reserve(ids.size());
        for (const T id : ids)
            if (id != 0 && present.insert(id).second)
                missing.push_back(id);

        if (!missing.empty())
        {
            if (!CUtlMemoryGrowWrap(pVec, static_cast<uint32>(missing.size())))
            {
                if (pChanged) *pChanged = changed;
                return false;
            }

            const uint32 oldSize = pVec->m_Size;
            for (uint32 i = 0; i < static_cast<uint32>(missing.size()); ++i)
                pVec->m_Memory.m_pMemory[oldSize + i] = missing[i];
            pVec->m_Size = oldSize + static_cast<uint32>(missing.size());
            changed = true;
        }

        if (pChanged) *pChanged = changed;
        return true;
    }

    template <typename T>
    bool RemoveAllIds(CUtlVector<T>* pVec, const std::unordered_set<T>& ids)
    {
        if (!pVec || (pVec->m_Size != 0 && !pVec->m_Memory.m_pMemory))
            return false;
        bool changed = false;
        for (uint32 i = 0; i < pVec->m_Size;)
        {
            if (ids.contains(pVec->m_Memory.m_pMemory[i]))
            {
                pVec->FastRemove(i);
                changed = true;
            }
            else
                ++i;
        }
        return changed;
    }

    bool SyncConfiguredLicenseVectors(PackageInfo* pPkg, bool* pChanged = nullptr)
    {
        if (pChanged) *pChanged = false;
        if (!pPkg) return false;

        const std::vector<AppId_t> configuredIds = LuaConfig::GetAllConfiguredIds();
        std::vector<AppId_t> appIds;
        std::vector<AppId_t> depotIds;
        appIds.reserve(configuredIds.size());
        depotIds.reserve(configuredIds.size());
        std::unordered_set<AppId_t> appInfoIds;
        std::unordered_set<AppId_t> depotOnlyIds;
        for (AppId_t id : configuredIds)
        {
            if (Hooks_Misc::IsAppInfoId(id))
            {
                appIds.push_back(id);
                appInfoIds.insert(id);
            }
            else
            {
                depotIds.push_back(id);
                depotOnlyIds.insert(id);
            }
        }

        bool appChanged = false;
        bool depotChanged = false;
        // Keep the package's app and depot domains disjoint. A real appinfo ID
        // belongs only in AppIdVec; a depot-only ID belongs only in DepotIdVec.
        const bool removedDepotIdsFromApps = RemoveAllIds(&pPkg->AppIdVec, depotOnlyIds);
        const bool removedAppIdsFromDepots = RemoveAllIds(&pPkg->DepotIdVec, appInfoIds);
        if (!AppendUniqueIds(&pPkg->AppIdVec, appIds, &appChanged))
            return false;
        if (!AppendUniqueIds(&pPkg->DepotIdVec, depotIds, &depotChanged))
            return false;

        if (pChanged) *pChanged = appChanged || depotChanged ||
                                  removedDepotIdsFromApps || removedAppIdsFromDepots;
        return true;
    }

    bool IsConfiguredDepot(AppId_t id)
    {
        // App IDs and depot IDs share a numeric namespace. Appinfo-backed
        // configured IDs must stay in AppIdVec for index bookkeeping, but that
        // must not stop an ID from satisfying the depot gate when Steam asks
        // about that same numeric depot ID.
        return id != 0 && LuaConfig::HasDepot(id, false);
    }

    std::vector<AppId_t> TakePendingRebuilds()
    {
        std::lock_guard lock(g_rebuildMutex);
        std::vector<AppId_t> pending(g_pendingRebuilds.begin(), g_pendingRebuilds.end());
        g_pendingRebuilds.clear();
        return pending;
    }

    void RequeuePendingRebuilds(const std::vector<AppId_t>& appIds)
    {
        std::lock_guard lock(g_rebuildMutex);
        g_pendingRebuilds.insert(appIds.begin(), appIds.end());
    }

    bool InitFakeLicenseOnce(PackageInfo* pPkg)
    {
        if (pPkg->Status != EPackageStatus::Available)
        {
            LOG_PACKAGE_WARN("InitFakeLicenseOnce: package status is not Available ({}), skipping injection",
                             static_cast<int>(pPkg->Status));
            return false;
        }

        bool changed = false;
        if (!SyncConfiguredLicenseVectors(pPkg, &changed))
        {
            LOG_PACKAGE_WARN("InitFakeLicenseOnce: failed to synchronize app/depot license vectors");
            return false;
        }

        g_licenseInitialized = true;
        // Rebuild at the end of the first complete vector sync. The observed
        // startup index pass can finish before package 0 is populated.
        g_licenseRefreshPending = true;
        TryProcessPendingLicenseRefresh();
        LOG_PACKAGE_INFO("InitFakeLicense(PackageId={}): synchronized app/depot vectors changed={} {}",
                         kInjectedPackageId, changed, PackageVectorsDebug(pPkg));
        return true;
    }

    bool TryInitFakeLicenseOnce()
    {
        if (g_licenseInitialized) return true;
        if (CAPTURE_READY(GetPackageInfo))
        {
            PackageInfo* pPkg = oGetPackageInfo(g_pCPackageInfo, kInjectedPackageId,
                                                kInjectedPkgAccessToken);
            if (!pPkg)
            {
                LOG_PACKAGE_WARN("TryInitFakeLicenseOnce: GetPackageInfo returned null for injected package");
                return false;
            }
            if (!g_pInjectedPackageInfo) g_pInjectedPackageInfo = pPkg;
            return InitFakeLicenseOnce(pPkg);
        }
        return false;
    }

    HOOK_FUNC(DepotLicenseGate, bool, void* pUser, AppId_t depotId, AppId_t appId)
    {
        const bool configuredDepot = IsConfiguredDepot(depotId);
        if (configuredDepot)
        {
            if (g_depotGateConfiguredLogCount.fetch_add(1, std::memory_order_relaxed) < 128)
                LOG_PACKAGE_INFO("DepotLicenseGate: granting configured depot={} app={} appinfo_id={} package_membership=[{}] user=0x{:X}",
                                 depotId, appId, Hooks_Misc::IsAppInfoId(depotId),
                                 PackageIdMembershipDebug(g_pInjectedPackageInfo, depotId),
                                 reinterpret_cast<uintptr_t>(pUser));
            LOG_PACKAGE_DEBUG("DepotLicenseGate: granted configured depot {} for app {}",
                              depotId, appId);
            return true;
        }

        const bool result = oDepotLicenseGate(pUser, depotId, appId);
        if (g_depotGatePassThroughLogCount.fetch_add(1, std::memory_order_relaxed) < 32)
            LOG_PACKAGE_INFO("DepotLicenseGate: passed through depot={} app={} original_result={} user=0x{:X}",
                             depotId, appId, result, reinterpret_cast<uintptr_t>(pUser));
        return result;
    }

    HOOK_FUNC(CheckAppOwnership, bool, void* pObj, AppId_t appId, AppOwnership* pOwn)
    {
        if (!g_pCUser)
        {
            g_pCUser = pObj;
            LOG_PACKAGE_DEBUG("CheckAppOwnership: captured CUser {}", g_pCUser);
        }

        const bool result = oCheckAppOwnership(pObj, appId, pOwn);
        const bool configuredApp = LuaConfig::HasDepot(appId, false);
        const bool logOwnership = configuredApp && pOwn &&
            g_ownershipDiagnosticCount.fetch_add(1, std::memory_order_relaxed) < 64;
        const PackageId_t originalPackageId = pOwn ? pOwn->PackageId : 0;
        const uint32 originalPackageCount = pOwn ? pOwn->ExistInPackageNums : 0;
        const EAppReleaseState originalReleaseState = pOwn ? pOwn->ReleaseState : EAppReleaseState{};
        const bool originalOwnsLicense = pOwn && pOwn->bOwnsLicense;
        const bool originalLicensePending = pOwn && pOwn->bLicensePending;
        if (logOwnership)
        {
            LOG_PACKAGE_WARN("CheckAppOwnership input: app={} result={} package={} package_count={} release_state={} owns_license={} license_pending={} appinfo_common_name={} initialized={} refresh_pending={} membership=[{}] vectors={}",
                             appId, result, originalPackageId, originalPackageCount,
                             static_cast<int>(originalReleaseState), originalOwnsLicense,
                             originalLicensePending, Hooks_Misc::IsAppInfoId(appId),
                             g_licenseInitialized, g_licenseRefreshPending,
                             PackageIdMembershipDebug(g_pInjectedPackageInfo, appId),
                             PackageVectorsDebug(g_pInjectedPackageInfo));
        }
        if (g_processingLicenseUpdate)
        {
            if (pOwn && LuaConfig::HasDepot(appId, false))
            {
                pOwn->PackageId = kInjectedPackageId;
                pOwn->ReleaseState = EAppReleaseState::Released;
                pOwn->bOwnsLicense = true;
                pOwn->bFreeLicense = false;
                if (logOwnership)
                    LOG_PACKAGE_WARN("CheckAppOwnership output during index rebuild: app={} package={} package_count={} owns_license={} license_pending={}",
                                     appId, pOwn->PackageId, pOwn->ExistInPackageNums,
                                     pOwn->bOwnsLicense, pOwn->bLicensePending);
                return true;
            }
            return result;
        }

        const bool initializedBefore = g_licenseInitialized;
        const bool refreshWasPending = g_licenseRefreshPending;
        TryInitFakeLicenseOnce();
        if (initializedBefore)
            TryProcessPendingLicenseRefresh();
        bool indexRebuilt = (!initializedBefore && g_licenseInitialized && !g_licenseRefreshPending) ||
                            (refreshWasPending && !g_licenseRefreshPending);

        bool vectorsChanged = false;
        if (g_licenseInitialized && g_pInjectedPackageInfo &&
            SyncConfiguredLicenseVectors(g_pInjectedPackageInfo, &vectorsChanged) && vectorsChanged)
        {
            // A PICS response may have changed a previously unknown ID into an
            // appinfo-backed app; remove it from DepotIdVec and rebuild once.
            if (!indexRebuilt && !g_licenseRefreshPending)
                indexRebuilt = MarkLicenseAsChangedAndProcessUpdates();
            if (!indexRebuilt)
                g_licenseRefreshPending = true;
        }

        const std::vector<AppId_t> pendingRebuilds = TakePendingRebuilds();
        if (!pendingRebuilds.empty())
        {
            if (!indexRebuilt)
            {
                if (g_licenseRefreshPending)
                {
                    RequeuePendingRebuilds(pendingRebuilds);
                }
                else if (MarkLicenseAsChangedAndProcessUpdates())
                {
                    indexRebuilt = true;
                    LOG_PACKAGE_INFO("CheckAppOwnership: rebuilt the package index for {} queued zero-depot request(s)",
                                     pendingRebuilds.size());
                }
                else
                {
                    g_licenseRefreshPending = true;
                    RequeuePendingRebuilds(pendingRebuilds);
                }
            }
        }

        if (pOwn && LuaConfig::HasDepot(appId, false))
        {
            if (result && pOwn->ExistInPackageNums > 1)
            {
                // This is a real license, not the package-0 entry we injected.
                LuaConfig::MarkOwned(appId);
                pOwn->ReleaseState = EAppReleaseState::Released;
                if (logOwnership)
                    LOG_PACKAGE_WARN("CheckAppOwnership output: app={} kept real license package={} package_count={} owns_license={} license_pending={}",
                                     appId, pOwn->PackageId, pOwn->ExistInPackageNums,
                                     pOwn->bOwnsLicense, pOwn->bLicensePending);
            }
            else
            {
                pOwn->PackageId = kInjectedPackageId;
                pOwn->ReleaseState = EAppReleaseState::Released;
                pOwn->bOwnsLicense = true;
                pOwn->bFreeLicense = false;
                if (logOwnership)
                    LOG_PACKAGE_WARN("CheckAppOwnership output: app={} assigned injected package={} package_count={} owns_license={} license_pending={} membership=[{}] vectors={}",
                                     appId, pOwn->PackageId, pOwn->ExistInPackageNums,
                                     pOwn->bOwnsLicense, pOwn->bLicensePending,
                                     PackageIdMembershipDebug(g_pInjectedPackageInfo, appId),
                                     PackageVectorsDebug(g_pInjectedPackageInfo));
                return true;
            }
        }
        return result;
    }
} // namespace

namespace Hooks_Package {
    void Install()
    {
        RESOLVE_C(CUtlMemoryGrow);
        RESOLVE_C(MarkLicenseAsChanged);
        RESOLVE_C(ProcessPendingLicenseUpdates);
        ARM_CAPTURE_C(GetPackageInfo);

        HOOK_BEGIN();
        INSTALL_HOOK_C(CheckAppOwnership);

        void* gateTarget = ResolveDepotLicenseGate();
        if (gateTarget)
        {
            oDepotLicenseGate = reinterpret_cast<DepotLicenseGate_t>(gateTarget);
            if (!OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oDepotLicenseGate),
                                             reinterpret_cast<void*>(hkDepotLicenseGate)))
            {
                oDepotLicenseGate = nullptr;
                _ost_detour_transaction_ok_ = false;
                LOG_PACKAGE_WARN("DepotLicenseGate detour attach failed");
            }
            else
            {
                LOG_PACKAGE_INFO("DepotLicenseGate detour attached: target=0x{:X} trampoline=0x{:X}",
                                 reinterpret_cast<uintptr_t>(gateTarget),
                                 reinterpret_cast<uintptr_t>(oDepotLicenseGate));
            }
        }
        else
        {
            LOG_PACKAGE_WARN("DepotLicenseGate not installed: TOML/.pdata validation failed");
        }
        HOOK_END();
    }

    void Uninstall()
    {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(CheckAppOwnership);
        if (oDepotLicenseGate &&
            OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oDepotLicenseGate),
                                        reinterpret_cast<void*>(hkDepotLicenseGate)))
            oDepotLicenseGate = nullptr;
        UNHOOK_END();
    }

    void RequestLicenseRebuild(AppId_t appId)
    {
        if (!appId || !LuaConfig::HasDepot(appId, false))
            return;

        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(g_rebuildMutex);
        const auto it = g_lastRebuildRequest.find(appId);
        if (it != g_lastRebuildRequest.end() && now - it->second < std::chrono::seconds(1))
            return;

        g_lastRebuildRequest[appId] = now;
        g_pendingRebuilds.insert(appId);
        LOG_PACKAGE_DEBUG("RequestLicenseRebuild: queued app {} (one-second per-app cooldown)", appId);
    }

    void NotifyLicenseChanged()
    {
        PackageInfo* pPkg = g_pInjectedPackageInfo;
        if (!pPkg)
        {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: injected PackageInfo not ready, cannot notify");
            return;
        }

        const std::vector<AppId_t> removals = LuaConfig::TakePendingRemovals();
        const std::vector<AppId_t> additions = LuaConfig::TakePendingAdditions();
        size_t removedCount = 0;
        for (AppId_t id : removals)
        {
            bool appRemoved = false;
            while (pPkg->AppIdVec.FindAndFastRemove(id))
                appRemoved = true;
            bool depotRemoved = false;
            while (pPkg->DepotIdVec.FindAndFastRemove(id))
                depotRemoved = true;
            if (appRemoved || depotRemoved)
                ++removedCount;
            else
                LOG_PACKAGE_DEBUG("NotifyLicenseChanged: ID {} was not present in either injected vector", id);
        }

        std::vector<AppId_t> appAdditions;
        std::vector<AppId_t> depotAdditions;
        appAdditions.reserve(additions.size());
        depotAdditions.reserve(additions.size());
        std::unordered_set<AppId_t> appInfoIds;
        std::unordered_set<AppId_t> depotOnlyIds;
        for (AppId_t id : additions)
        {
            if (Hooks_Misc::IsAppInfoId(id))
            {
                appAdditions.push_back(id);
                appInfoIds.insert(id);
            }
            else
            {
                depotAdditions.push_back(id);
                depotOnlyIds.insert(id);
            }
        }
        const bool removedDepotIdsFromApps = RemoveAllIds(&pPkg->AppIdVec, depotOnlyIds);
        const bool removedAppIdsFromDepots = RemoveAllIds(&pPkg->DepotIdVec, appInfoIds);

        bool appsAdded = false;
        bool depotsAdded = false;
        const bool appAppendOk = AppendUniqueIds(&pPkg->AppIdVec, appAdditions, &appsAdded);
        const bool depotAppendOk = AppendUniqueIds(&pPkg->DepotIdVec, depotAdditions, &depotsAdded);
        appsAdded = appsAdded || removedDepotIdsFromApps;
        depotsAdded = depotsAdded || removedAppIdsFromDepots;
        if (!appAppendOk || !depotAppendOk)
        {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: append failed (apps_ok={}, depots_ok={}); next ownership check will retry sync",
                             appAppendOk, depotAppendOk);
        }

        std::unordered_set<AppId_t> addedIds(additions.begin(), additions.end());
        for (AppId_t id : additions)
            Hooks_SteamUI::CancelRemoval(id);

        if (!appsAdded && !depotsAdded && !removedCount)
        {
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: no vector changes");
            return;
        }

        if (!MarkLicenseAsChangedAndProcessUpdates())
        {
            g_licenseRefreshPending = true;
            LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to mark license as changed");
            return;
        }
        LOG_PACKAGE_INFO("NotifyLicenseChanged: {} app IDs added, {} depot IDs added, {} IDs removed",
                         appsAdded ? appAdditions.size() : 0, depotsAdded ? depotAdditions.size() : 0,
                         removedCount);

        size_t queuedRemovalCount = 0;
        for (AppId_t id : removals)
        {
            if (!addedIds.contains(id))
            {
                Hooks_SteamUI::QueueRemoval(id);
                ++queuedRemovalCount;
            }
        }
        LOG_PACKAGE_DEBUG("NotifyLicenseChanged: queued {} UI removals, skipped {} transient removals",
                          queuedRemovalCount, removals.size() - queuedRemovalCount);
    }
} // namespace Hooks_Package
