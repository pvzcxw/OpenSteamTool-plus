#include "Hooks_SteamUI.h"
#include "HookManager.h"
#include "HookMacros.h"
#include "Hooks_Diversion.h"
#include "dllmain.h"
#include "steam_messages.pb.h"
#include "OSTPlatform/include/Detour.h"
#include "OSTPlatform/include/Memory.h"
#include "Utils/HookSupport/VehCommon.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
    // These SteamUI entry-point signatures are intentionally self-contained:
    // PatternLoader can block on remote metadata, but the module-loader hook
    // must be ready before the first steamclient request.
    constexpr std::array<uint8_t, 13> kLoadModuleSignatureA{
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40};
    constexpr std::array<uint8_t, 19> kLoadModuleSignatureB{
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20,
        0x56, 0x41, 0x54, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40};

    static bool IsSteamClient64Path(const char* path) noexcept
    {
        if (!path) return false;
        const std::string_view value(path);
        const size_t slash = value.find_last_of("\\/");
        const std::string_view file = slash == std::string_view::npos ? value : value.substr(slash + 1);
        return (file.size() == 17 && _strnicmp(file.data(), "steamclient64.dll", 17) == 0) ||
               (file.size() == 13 && _strnicmp(file.data(), "steamclient64", 13) == 0);
    }

    template <size_t N>
    static void FindSignatureMatches(const uint8_t* start, size_t length,
                                     const std::array<uint8_t, N>& signature,
                                     std::vector<void*>& matches)
    {
        if (!start || length < N) return;
        const uint8_t* cursor = start;
        const uint8_t* const end = start + length;
        while (cursor < end) {
            const auto* candidate = static_cast<const uint8_t*>(
                std::memchr(cursor, signature[0], static_cast<size_t>(end - cursor)));
            if (!candidate) return;
            if (static_cast<size_t>(end - candidate) >= N &&
                std::memcmp(candidate, signature.data(), 4) == 0 &&
                std::memcmp(candidate, signature.data(), N) == 0 &&
                std::find(matches.begin(), matches.end(), const_cast<uint8_t*>(candidate)) == matches.end()) {
                matches.push_back(const_cast<uint8_t*>(candidate));
            }
            cursor = candidate + 1;
        }
    }

    static void* FindLoadModuleWithPathInText(OSTPlatform::DynamicLibrary::ModuleHandle module) noexcept
    {
        const auto image = OSTPlatform::Memory::GetModuleImage(module);
        if (!image || !image->base || image->size < sizeof(IMAGE_DOS_HEADER)) return nullptr;

        const auto* base = image->base;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;

        const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
        constexpr size_t ntHeaderSize = offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + sizeof(IMAGE_OPTIONAL_HEADER64);
        if (ntOffset > image->size || ntHeaderSize > image->size - ntOffset) return nullptr;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + ntOffset);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt->OptionalHeader.SizeOfImage > image->size) {
            return nullptr;
        }

        const size_t sectionOffset = ntOffset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                     nt->FileHeader.SizeOfOptionalHeader;
        const size_t sectionCount = nt->FileHeader.NumberOfSections;
        if (sectionOffset > image->size ||
            sectionCount > (image->size - sectionOffset) / sizeof(IMAGE_SECTION_HEADER)) {
            return nullptr;
        }

        const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(base + sectionOffset);
        for (size_t i = 0; i < sectionCount; ++i) {
            const auto& section = sections[i];
            if (std::memcmp(section.Name, ".text", 5) != 0 ||
                !(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
                continue;
            }

            const size_t rva = section.VirtualAddress;
            const size_t textSize = section.Misc.VirtualSize ? section.Misc.VirtualSize : section.SizeOfRawData;
            if (rva >= image->size || textSize > image->size - rva) return nullptr;

            std::vector<void*> matches;
            try {
                FindSignatureMatches(base + rva, textSize, kLoadModuleSignatureA, matches);
                FindSignatureMatches(base + rva, textSize, kLoadModuleSignatureB, matches);
            } catch (...) {
                return nullptr;
            }
            if (matches.size() == 1) return matches.front();
            if (matches.size() > 1) {
                LOG_WARN("SteamUI loader signature matched {} addresses; skipping ambiguous hook", matches.size());
            }
            return nullptr;
        }
        return nullptr;
    }

    HOOK_FUNC(LoadModuleWithPath, HMODULE, const char* path, bool flags)
    {
        if (IsSteamClient64Path(path)) {
            if (HMODULE client = Hooks_Diversion::AcquireClientModuleReference()) {
                if (!g_HooksInstalled.load(std::memory_order_acquire) &&
                    Hooks_Diversion::g_hClientHooksReadyEvent) {
                    const DWORD wait = ::WaitForSingleObject(
                        Hooks_Diversion::g_hClientHooksReadyEvent, 5000);
                    if (wait != WAIT_OBJECT_0) {
                        LOG_WARN("LoadModuleWithPath: client-hook readiness wait ended with {} (error={}); continuing with selected client",
                                 wait, ::GetLastError());
                    }
                }
                LOG_INFO("LoadModuleWithPath: redirected {} to {}",
                         path, g_IsDiversionActive.load(std::memory_order_acquire)
                                   ? DiversionPath : SteamclientPath);
                return client;
            }
            LOG_WARN("LoadModuleWithPath: diversion module unavailable for {}; forwarding to SteamUI loader",
                     path ? path : "(null)");
        }
        return oLoadModuleWithPath(path, flags);
    }

    RESOLVE_FUNC(RepeatedFieldUint32_Add, void, void* field, const uint32* value);

    CAPTURE_THIS_FUNC(GetAppByID, CSteamApp*, g_pController,void* pThis, AppId_t appId, bool bCreate);
    CAPTURE_THIS_FUNC(MarkAppChange,void*,g_pAppChangeSource,void* pThis,AppId_t appId, EAppChangeFlags changeFlags);

    HOOK_FUNC(FillInAppOverview, void *, void *pThis, void *pAppOverview, CSteamApp *pApp)
    {
        if (pApp && LuaConfig::HasDepot(pApp->nAppID, false))
        {
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
    bool InstallModuleLoader() noexcept
    {
        if (oLoadModuleWithPath) return true;

        const auto module = ui_hModule.load(std::memory_order_acquire);
        if (!module) {
            LOG_WARN("SteamUI loader hook skipped: steamui.dll is not loaded");
            return false;
        }

        void* target = FindLoadModuleWithPathInText(module);
        if (!target) {
            LOG_WARN("SteamUI loader hook skipped: no unique built-in signature match");
            return false;
        }
        if (!OSTPlatform::Detour::BeginTransaction()) {
            LOG_WARN("SteamUI loader hook skipped: Detours transaction could not start");
            return false;
        }

        oLoadModuleWithPath = reinterpret_cast<LoadModuleWithPath_t>(target);
        const bool attached = OSTPlatform::Detour::Attach(
            reinterpret_cast<void**>(&oLoadModuleWithPath), reinterpret_cast<void*>(hkLoadModuleWithPath));
        const bool committed = OSTPlatform::Detour::CommitTransaction();
        if (!attached || !committed) {
            oLoadModuleWithPath = nullptr;
            LOG_WARN("SteamUI loader hook installation failed (attach={}, commit={})", attached, committed);
            return false;
        }

        LOG_INFO("SteamUI LoadModuleWithPath hook installed from built-in .text signature");
        return true;
    }

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
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(LoadModuleWithPath);
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
