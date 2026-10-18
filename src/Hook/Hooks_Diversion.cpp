#include "Hooks_Diversion.h"
#include "OSTPlatform/include/Detour.h"
#include "OSTPlatform/include/Encoding.h"
#include "Utils/Logging/Log.h"
#include "dllmain.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <windows.h>

namespace Hooks_Diversion {

namespace {

using LoadLibraryExW_t     = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using GetModuleHandleW_t   = HMODULE(WINAPI*)(LPCWSTR);
using GetModuleHandleA_t   = HMODULE(WINAPI*)(LPCSTR);
using GetModuleHandleExW_t = BOOL(WINAPI*)(DWORD, LPCWSTR, HMODULE*);
using GetModuleHandleExA_t = BOOL(WINAPI*)(DWORD, LPCSTR, HMODULE*);

LoadLibraryExW_t     s_origLoadLibraryExW     = nullptr;
GetModuleHandleW_t   s_origGetModuleHandleW   = nullptr;
GetModuleHandleA_t   s_origGetModuleHandleA   = nullptr;
GetModuleHandleExW_t s_origGetModuleHandleExW = nullptr;
GetModuleHandleExA_t s_origGetModuleHandleExA = nullptr;

std::atomic<bool> s_isInstalled{false};
std::atomic<bool> s_loadLibraryExWHooked{false};
std::atomic<bool> s_getModuleHandleWHooked{false};
std::atomic<bool> s_getModuleHandleAHooked{false};
std::atomic<bool> s_getModuleHandleExWHooked{false};
std::atomic<bool> s_getModuleHandleExAHooked{false};
std::wstring s_wideDiversionPath;
std::wstring s_wideOriginalPath;
std::mutex s_pathMutex;
bool s_pathsConfigured = false;
std::mutex s_clientLoadMutex;
std::atomic<bool> s_diversionFileReady{false};

constexpr DWORD kDiversionFileReadyWaitMs = 4000;
constexpr unsigned kDiversionLoadAttempts = 20;
constexpr DWORD kDiversionLoadRetryMs = 50;

#ifdef OPENSTEAMTOOL_LOGGING_ENABLED
#define DIV_LOG_INFO(...)  do { if (Log::Main) LOG_INFO(__VA_ARGS__); } while (0)
#define DIV_LOG_WARN(...)  do { if (Log::Main) LOG_WARN(__VA_ARGS__); } while (0)
#define DIV_LOG_ERROR(...) do { if (Log::Main) LOG_ERROR(__VA_ARGS__); } while (0)
#define DIV_LOG_DEBUG(...) do { if (Log::Main) LOG_DEBUG(__VA_ARGS__); } while (0)
#else
#define DIV_LOG_INFO(...)  ((void)0)
#define DIV_LOG_WARN(...)  ((void)0)
#define DIV_LOG_ERROR(...) ((void)0)
#define DIV_LOG_DEBUG(...) ((void)0)
#endif

[[nodiscard]] inline bool IsSteamClientModuleW(std::wstring_view path) noexcept {
    if (path.empty()) return false;
    const auto pos = path.find_last_of(L"\\/");
    const std::wstring_view fn = (pos == std::wstring_view::npos) ? path : path.substr(pos + 1);

    switch (fn.size()) {
        case 17: return _wcsnicmp(fn.data(), L"steamclient64.dll", 17) == 0;
        case 15: return _wcsnicmp(fn.data(), L"steamclient.dll",   15) == 0;
        case 13: return _wcsnicmp(fn.data(), L"steamclient64",     13) == 0;
        case 11: return _wcsnicmp(fn.data(), L"steamclient",       11) == 0;
        default: return false;
    }
}

[[nodiscard]] inline bool IsSteamClientModuleA(std::string_view path) noexcept {
    if (path.empty()) return false;
    const auto pos = path.find_last_of("\\/");
    const std::string_view fn = (pos == std::string_view::npos) ? path : path.substr(pos + 1);

    switch (fn.size()) {
        case 17: return _strnicmp(fn.data(), "steamclient64.dll", 17) == 0;
        case 15: return _strnicmp(fn.data(), "steamclient.dll",   15) == 0;
        case 13: return _strnicmp(fn.data(), "steamclient64",     13) == 0;
        case 11: return _strnicmp(fn.data(), "steamclient",       11) == 0;
        default: return false;
    }
}

void WaitForDiversionFileReady() noexcept {
    if (s_diversionFileReady.load(std::memory_order_acquire)) return;
    if (g_hDiversionFileReadyEvent) {
        ::WaitForSingleObject(g_hDiversionFileReadyEvent, kDiversionFileReadyWaitMs);
        return;
    }
    for (DWORD waited = 0; waited < kDiversionFileReadyWaitMs; waited += kDiversionLoadRetryMs) {
        if (s_diversionFileReady.load(std::memory_order_acquire)) return;
        ::Sleep(kDiversionLoadRetryMs);
    }
}

HMODULE CallOriginalLoadLibraryExW(LPCWSTR path, HANDLE file, DWORD flags) noexcept {
    if (!path) return nullptr;
    if (s_origLoadLibraryExW) return s_origLoadLibraryExW(path, file, flags);
    return ::LoadLibraryExW(path, file, flags);
}

HMODULE GetAlreadyLoadedOriginalClient() noexcept {
    if (s_origGetModuleHandleW) return s_origGetModuleHandleW(L"steamclient64.dll");
    return ::GetModuleHandleW(L"steamclient64.dll");
}

HMODULE EnsureClientModuleLoadedLocked() noexcept {
    auto current = client_hModule.load(std::memory_order_acquire);
    if (current) return reinterpret_cast<HMODULE>(current);

    // Steam may have mapped the original before the payload arrived. Keep that
    // module as the explicit fallback rather than destabilizing initialization.
    if (HMODULE original = GetAlreadyLoadedOriginalClient()) {
        client_hModule.store(reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(original),
                             std::memory_order_release);
        g_IsDiversionActive.store(false, std::memory_order_release);
        DIV_LOG_WARN("Hooks_Diversion: original steamclient64.dll was already loaded; keeping original fallback");
        return original;
    }

    if (g_IsDiversionActive.load(std::memory_order_acquire) && !s_wideDiversionPath.empty()) {
        for (unsigned attempt = 0; attempt < kDiversionLoadAttempts; ++attempt) {
            HMODULE shadow = CallOriginalLoadLibraryExW(
                s_wideDiversionPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (shadow) {
                client_hModule.store(reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(shadow),
                                     std::memory_order_release);
                g_IsDiversionActive.store(true, std::memory_order_release);
                DIV_LOG_INFO("Hooks_Diversion: loaded shadow client after {} attempt(s)", attempt + 1);
                return shadow;
            }
            if (attempt + 1 < kDiversionLoadAttempts) ::Sleep(kDiversionLoadRetryMs);
        }
        DIV_LOG_WARN("Hooks_Diversion: shadow load exhausted {} attempts (error={}); trying original client",
                     kDiversionLoadAttempts, ::GetLastError());
    }

    g_IsDiversionActive.store(false, std::memory_order_release);
    if (!s_wideOriginalPath.empty()) {
        HMODULE original = CallOriginalLoadLibraryExW(
            s_wideOriginalPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (original) {
            client_hModule.store(reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(original),
                                 std::memory_order_release);
            DIV_LOG_WARN("Hooks_Diversion: loaded original steamclient64.dll fallback");
            return original;
        }
    }
    return nullptr;
}

HMODULE LoadClientModuleForSteam(LPCWSTR requestedPath, HANDLE hFile, DWORD flags) noexcept {
    // Never hold the module mutex while waiting for the worker that creates the copy.
    WaitForDiversionFileReady();

    HMODULE loaded = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_clientLoadMutex);
        HMODULE base = EnsureClientModuleLoadedLocked();
        const bool useShadow = base && g_IsDiversionActive.load(std::memory_order_acquire) &&
                               !s_wideDiversionPath.empty();
        LPCWSTR loadPath = useShadow ? s_wideDiversionPath.c_str() : requestedPath;
        loaded = CallOriginalLoadLibraryExW(loadPath, useShadow ? nullptr : hFile, flags);
        if (!loaded && useShadow) {
            DIV_LOG_WARN("Hooks_Diversion: per-request shadow load failed (error={}); retrying requested original path",
                         ::GetLastError());
            g_IsDiversionActive.store(false, std::memory_order_release);
            loaded = CallOriginalLoadLibraryExW(requestedPath, hFile, flags);
            if (loaded) client_hModule.store(
                reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(loaded),
                std::memory_order_release);
        } else if (!loaded && !base) {
            loaded = CallOriginalLoadLibraryExW(requestedPath, hFile, flags);
            if (loaded) {
                client_hModule.store(reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(loaded),
                                     std::memory_order_release);
                g_IsDiversionActive.store(false, std::memory_order_release);
            }
        }
    }

    // The barrier deliberately follows the load mutex to avoid serializing any
    // module-load caller behind InitThread's hook installation.
    if (loaded && !g_HooksInstalled.load(std::memory_order_acquire) && g_hClientHooksReadyEvent) {
        ::WaitForSingleObject(g_hClientHooksReadyEvent, 2000);
    }
    return loaded;
}

HMODULE WINAPI Hooked_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (lpLibFileName && IsSteamClientModuleW(lpLibFileName)) {
        HMODULE client = LoadClientModuleForSteam(lpLibFileName, hFile, dwFlags);
        if (client) {
            DIV_LOG_INFO("Hooks_Diversion: redirected Steam client request {} -> {}",
                         OSTPlatform::Encoding::WideToUtf8(lpLibFileName),
                         g_IsDiversionActive.load(std::memory_order_acquire) ? DiversionPath : SteamclientPath);
            return client;
        }
        DIV_LOG_ERROR("Hooks_Diversion: diversion and original client loads failed; returning loader failure");
        return nullptr;
    }
    return CallOriginalLoadLibraryExW(lpLibFileName, hFile, dwFlags);
}

HMODULE WINAPI Hooked_GetModuleHandleW(LPCWSTR lpModuleName) {
    if (lpModuleName && IsSteamClientModuleW(lpModuleName)) {
        auto h = client_hModule.load(std::memory_order_acquire);
        if (h) return reinterpret_cast<HMODULE>(h);
        h = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(s_origGetModuleHandleW(lpModuleName));
        if (h) {
            client_hModule.store(h, std::memory_order_release);
            g_IsDiversionActive.store(false, std::memory_order_release);
            return reinterpret_cast<HMODULE>(h);
        }
    }
    return s_origGetModuleHandleW(lpModuleName);
}

HMODULE WINAPI Hooked_GetModuleHandleA(LPCSTR lpModuleName) {
    if (lpModuleName && IsSteamClientModuleA(lpModuleName)) {
        auto h = client_hModule.load(std::memory_order_acquire);
        if (h) return reinterpret_cast<HMODULE>(h);
        h = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(s_origGetModuleHandleA(lpModuleName));
        if (h) {
            client_hModule.store(h, std::memory_order_release);
            g_IsDiversionActive.store(false, std::memory_order_release);
            return reinterpret_cast<HMODULE>(h);
        }
    }
    return s_origGetModuleHandleA(lpModuleName);
}

BOOL WINAPI Hooked_GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule) {
    if (lpModuleName && !(dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
        IsSteamClientModuleW(lpModuleName))
    {
        auto h = client_hModule.load(std::memory_order_acquire);
        if (h) {
            if (phModule) {
                *phModule = reinterpret_cast<HMODULE>(h);
                if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) {
                    HMODULE dummy = nullptr;
                    LPCWSTR selectedPath = g_IsDiversionActive.load(std::memory_order_acquire)
                        ? s_wideDiversionPath.c_str() : s_wideOriginalPath.c_str();
                    if (selectedPath && *selectedPath)
                        s_origGetModuleHandleExW(dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN,
                                                 selectedPath, &dummy);
                }
                return TRUE;
            }
            ::SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }
    return s_origGetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

BOOL WINAPI Hooked_GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule) {
    if (lpModuleName && !(dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
        IsSteamClientModuleA(lpModuleName))
    {
        auto h = client_hModule.load(std::memory_order_acquire);
        if (h) {
            if (phModule) {
                *phModule = reinterpret_cast<HMODULE>(h);
                if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) {
                    HMODULE dummy = nullptr;
                    LPCSTR selectedPath = g_IsDiversionActive.load(std::memory_order_acquire)
                        ? DiversionPath : SteamclientPath;
                    if (selectedPath && *selectedPath)
                        s_origGetModuleHandleExA(dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN,
                                                 selectedPath, &dummy);
                }
                return TRUE;
            }
            ::SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }
    return s_origGetModuleHandleExA(dwFlags, lpModuleName, phModule);
}

} // namespace

void ConfigurePaths(const std::wstring& originalPath, const std::wstring& diversionPath) noexcept {
    try {
        std::lock_guard<std::mutex> lock(s_pathMutex);
        // Hooks read these immutable strings without acquiring a lock. Configure
        // once before Install() publishes any detour, then keep them stable.
        if (s_pathsConfigured) return;
        if (originalPath.empty() || diversionPath.empty()) return;
        s_wideOriginalPath = originalPath;
        s_wideDiversionPath = diversionPath;
        s_pathsConfigured = true;
    } catch (...) {
        std::lock_guard<std::mutex> lock(s_pathMutex);
        s_wideOriginalPath.clear();
        s_wideDiversionPath.clear();
        s_pathsConfigured = false;
        g_IsDiversionActive.store(false, std::memory_order_release);
    }
}

bool EnsureDiversionFileReady(const std::wstring& originalPath, const std::wstring& diversionPath) noexcept {
    auto signalReady = [](bool ready) {
        g_IsDiversionActive.store(ready, std::memory_order_release);
        s_diversionFileReady.store(true, std::memory_order_release);
        if (g_hDiversionFileReadyEvent) ::SetEvent(g_hDiversionFileReadyEvent);
    };

    if (originalPath.empty() || diversionPath.empty()) {
        signalReady(false);
        return false;
    }
    ConfigurePaths(originalPath, diversionPath);

    WIN32_FILE_ATTRIBUTE_DATA origAttr{}, divAttr{};
    const bool origExists = ::GetFileAttributesExW(originalPath.c_str(), GetFileExInfoStandard, &origAttr) != 0 &&
                            !(origAttr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    const bool divExists  = ::GetFileAttributesExW(diversionPath.c_str(), GetFileExInfoStandard, &divAttr) != 0 &&
                            !(divAttr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);

    if (!origExists) {
        DIV_LOG_WARN("Hooks_Diversion: Original steamclient64.dll not found at {}",
                     OSTPlatform::Encoding::WideToUtf8(originalPath));
        signalReady(false);
        return false;
    }

    const bool isUpToDate = divExists &&
        (origAttr.nFileSizeHigh != 0 || origAttr.nFileSizeLow != 0) &&
        origAttr.nFileSizeHigh == divAttr.nFileSizeHigh &&
        origAttr.nFileSizeLow == divAttr.nFileSizeLow &&
        origAttr.ftLastWriteTime.dwLowDateTime == divAttr.ftLastWriteTime.dwLowDateTime &&
        origAttr.ftLastWriteTime.dwHighDateTime == divAttr.ftLastWriteTime.dwHighDateTime;

    if (isUpToDate) {
        DIV_LOG_DEBUG("Hooks_Diversion: shadow module is already up to date ({})", DiversionPath);
        signalReady(true);
        return true;
    }

    try {
        const std::filesystem::path divFs(diversionPath);
        std::error_code ec;
        std::filesystem::create_directories(divFs.parent_path(), ec);
        if (divExists && (divAttr.dwFileAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
            ::SetFileAttributesW(diversionPath.c_str(), FILE_ATTRIBUTE_NORMAL);

        if (::CopyFileW(originalPath.c_str(), diversionPath.c_str(), FALSE)) {
            DIV_LOG_INFO("Hooks_Diversion: cloned steamclient64.dll -> diversion64.dll");
            signalReady(true);
            return true;
        }
        DIV_LOG_ERROR("Hooks_Diversion: CopyFileW failed with error {}", ::GetLastError());
    } catch (...) {
        DIV_LOG_ERROR("Hooks_Diversion: exception during shadow file preparation");
    }

    signalReady(false);
    return false;
}

HMODULE EnsureClientModuleLoaded() noexcept {
    WaitForDiversionFileReady();
    std::lock_guard<std::mutex> lock(s_clientLoadMutex);
    return EnsureClientModuleLoadedLocked();
}

HMODULE AcquireClientModuleReference() noexcept {
    WaitForDiversionFileReady();
    std::lock_guard<std::mutex> lock(s_clientLoadMutex);

    HMODULE client = EnsureClientModuleLoadedLocked();
    if (!client) return nullptr;

    const bool useShadow = g_IsDiversionActive.load(std::memory_order_acquire) &&
                           !s_wideDiversionPath.empty();
    const std::wstring& selectedPath = useShadow ? s_wideDiversionPath : s_wideOriginalPath;
    if (!selectedPath.empty()) {
        HMODULE referenced = CallOriginalLoadLibraryExW(
            selectedPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (referenced == client) return referenced;
        if (referenced) {
            // A full path should resolve to the module selected above.  Do not
            // return a different image to Steam if the loader state changed.
            ::FreeLibrary(referenced);
        }
    }

    // Keep ownership semantics even if reopening the path fails (for example,
    // a concurrent Steam update replaced the file after the first mapping).
    HMODULE referenced = nullptr;
    auto getModuleHandleEx = s_origGetModuleHandleExW
        ? s_origGetModuleHandleExW : ::GetModuleHandleExW;
    if (getModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          reinterpret_cast<LPCWSTR>(client), &referenced) &&
        referenced == client) {
        return referenced;
    }
    return nullptr;
}

HMODULE GetOriginalClientIfLoaded() noexcept {
    // s_origGetModuleHandleW is the trampoline retained by Detours.  Calling
    // it avoids the name redirection in Hooked_GetModuleHandleW and therefore
    // reports only a module that the Windows loader actually mapped.
    return GetAlreadyLoadedOriginalClient();
}

void Install() noexcept {
    if (s_isInstalled.load(std::memory_order_acquire)) {
        return;
    }

    if (!g_hClientHooksReadyEvent)
        g_hClientHooksReadyEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_hDiversionFileReadyEvent)
        g_hDiversionFileReadyEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

    HMODULE hModule = ::GetModuleHandleW(L"kernelbase.dll");
    if (!hModule) {
        hModule = ::GetModuleHandleW(L"kernel32.dll");
    }
    if (!hModule) {
        DIV_LOG_ERROR("Hooks_Diversion: Failed to locate kernelbase.dll or kernel32.dll");
        return;
    }

    auto pfnLoadLibraryExW = reinterpret_cast<LoadLibraryExW_t>(::GetProcAddress(hModule, "LoadLibraryExW"));
    auto pfnGetModuleHandleW = reinterpret_cast<GetModuleHandleW_t>(::GetProcAddress(hModule, "GetModuleHandleW"));
    auto pfnGetModuleHandleA = reinterpret_cast<GetModuleHandleA_t>(::GetProcAddress(hModule, "GetModuleHandleA"));
    auto pfnGetModuleHandleExW = reinterpret_cast<GetModuleHandleExW_t>(::GetProcAddress(hModule, "GetModuleHandleExW"));
    auto pfnGetModuleHandleExA = reinterpret_cast<GetModuleHandleExA_t>(::GetProcAddress(hModule, "GetModuleHandleExA"));

    if (!pfnLoadLibraryExW || !pfnGetModuleHandleW || !pfnGetModuleHandleA || !pfnGetModuleHandleExW || !pfnGetModuleHandleExA) {
        DIV_LOG_ERROR("Hooks_Diversion: Failed to locate module management entrypoints");
        return;
    }

    s_origLoadLibraryExW     = pfnLoadLibraryExW;
    s_origGetModuleHandleW   = pfnGetModuleHandleW;
    s_origGetModuleHandleA   = pfnGetModuleHandleA;
    s_origGetModuleHandleExW = pfnGetModuleHandleExW;
    s_origGetModuleHandleExA = pfnGetModuleHandleExA;

    if (!OSTPlatform::Detour::BeginTransaction()) {
        DIV_LOG_ERROR("Hooks_Diversion: Failed to begin detour transaction");
        return;
    }

    const bool loadLibraryAttached = OSTPlatform::Detour::Attach(
        reinterpret_cast<void**>(&s_origLoadLibraryExW), reinterpret_cast<void*>(Hooked_LoadLibraryExW));
    const bool getHandleWAttached = OSTPlatform::Detour::Attach(
        reinterpret_cast<void**>(&s_origGetModuleHandleW), reinterpret_cast<void*>(Hooked_GetModuleHandleW));
    const bool getHandleAAttached = OSTPlatform::Detour::Attach(
        reinterpret_cast<void**>(&s_origGetModuleHandleA), reinterpret_cast<void*>(Hooked_GetModuleHandleA));
    const bool getHandleExWAttached = OSTPlatform::Detour::Attach(
        reinterpret_cast<void**>(&s_origGetModuleHandleExW), reinterpret_cast<void*>(Hooked_GetModuleHandleExW));
    const bool getHandleExAAttached = OSTPlatform::Detour::Attach(
        reinterpret_cast<void**>(&s_origGetModuleHandleExA), reinterpret_cast<void*>(Hooked_GetModuleHandleExA));
    const bool committed = OSTPlatform::Detour::CommitTransaction();

    if (!committed) {
        DIV_LOG_ERROR("Hooks_Diversion: Failed to commit detour transaction");
    }
    s_loadLibraryExWHooked.store(loadLibraryAttached && committed, std::memory_order_release);
    s_getModuleHandleWHooked.store(getHandleWAttached && committed, std::memory_order_release);
    s_getModuleHandleAHooked.store(getHandleAAttached && committed, std::memory_order_release);
    s_getModuleHandleExWHooked.store(getHandleExWAttached && committed, std::memory_order_release);
    s_getModuleHandleExAHooked.store(getHandleExAAttached && committed, std::memory_order_release);

    const bool anyAttached = s_loadLibraryExWHooked.load(std::memory_order_acquire) ||
        s_getModuleHandleWHooked.load(std::memory_order_acquire) ||
        s_getModuleHandleAHooked.load(std::memory_order_acquire) ||
        s_getModuleHandleExWHooked.load(std::memory_order_acquire) ||
        s_getModuleHandleExAHooked.load(std::memory_order_acquire);
    s_isInstalled.store(anyAttached, std::memory_order_release);
    if (!loadLibraryAttached || !getHandleWAttached || !getHandleAAttached ||
        !getHandleExWAttached || !getHandleExAAttached) {
        DIV_LOG_ERROR("Hooks_Diversion: One or more module-management detours could not be attached");
    }
    if (anyAttached) {
        DIV_LOG_INFO("Hooks_Diversion: Chokepoint detours installed (LoadLibraryExW + GetModuleHandle family)");
    }
}

void Uninstall() noexcept {
    if (!s_isInstalled.load(std::memory_order_acquire)) {
        if (g_hClientHooksReadyEvent) {
            ::CloseHandle(g_hClientHooksReadyEvent);
            g_hClientHooksReadyEvent = nullptr;
        }
        if (g_hDiversionFileReadyEvent) {
            ::CloseHandle(g_hDiversionFileReadyEvent);
            g_hDiversionFileReadyEvent = nullptr;
        }
        return;
    }

    if (!OSTPlatform::Detour::BeginTransaction()) {
        DIV_LOG_ERROR("Hooks_Diversion: Failed to begin detour removal transaction");
        return;
    }

    const bool loadLibraryDetached = !s_loadLibraryExWHooked.load(std::memory_order_acquire) ||
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&s_origLoadLibraryExW), reinterpret_cast<void*>(Hooked_LoadLibraryExW));
    const bool getHandleWDetached = !s_getModuleHandleWHooked.load(std::memory_order_acquire) ||
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&s_origGetModuleHandleW), reinterpret_cast<void*>(Hooked_GetModuleHandleW));
    const bool getHandleADetached = !s_getModuleHandleAHooked.load(std::memory_order_acquire) ||
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&s_origGetModuleHandleA), reinterpret_cast<void*>(Hooked_GetModuleHandleA));
    const bool getHandleExWDetached = !s_getModuleHandleExWHooked.load(std::memory_order_acquire) ||
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&s_origGetModuleHandleExW), reinterpret_cast<void*>(Hooked_GetModuleHandleExW));
    const bool getHandleExADetached = !s_getModuleHandleExAHooked.load(std::memory_order_acquire) ||
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&s_origGetModuleHandleExA), reinterpret_cast<void*>(Hooked_GetModuleHandleExA));
    const bool committed = OSTPlatform::Detour::CommitTransaction();
    if (!committed) {
        DIV_LOG_ERROR("Hooks_Diversion: Failed to commit detour removal transaction");
        return;
    }

    s_loadLibraryExWHooked.store(!loadLibraryDetached, std::memory_order_release);
    s_getModuleHandleWHooked.store(!getHandleWDetached, std::memory_order_release);
    s_getModuleHandleAHooked.store(!getHandleADetached, std::memory_order_release);
    s_getModuleHandleExWHooked.store(!getHandleExWDetached, std::memory_order_release);
    s_getModuleHandleExAHooked.store(!getHandleExADetached, std::memory_order_release);
    const bool stillAttached = s_loadLibraryExWHooked.load(std::memory_order_acquire) ||
        s_getModuleHandleWHooked.load(std::memory_order_acquire) ||
        s_getModuleHandleAHooked.load(std::memory_order_acquire) ||
        s_getModuleHandleExWHooked.load(std::memory_order_acquire) ||
        s_getModuleHandleExAHooked.load(std::memory_order_acquire);
    s_isInstalled.store(stillAttached, std::memory_order_release);

    if (!stillAttached) {
        if (g_hClientHooksReadyEvent) {
            ::CloseHandle(g_hClientHooksReadyEvent);
            g_hClientHooksReadyEvent = nullptr;
        }
        if (g_hDiversionFileReadyEvent) {
            ::CloseHandle(g_hDiversionFileReadyEvent);
            g_hDiversionFileReadyEvent = nullptr;
        }
    }
    DIV_LOG_INFO("Hooks_Diversion: Chokepoint detours unhooked cleanly");
}

bool IsInstalled() noexcept {
    return s_isInstalled.load(std::memory_order_acquire);
}

} // namespace Hooks_Diversion
