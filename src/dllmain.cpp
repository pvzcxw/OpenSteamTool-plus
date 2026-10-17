#include "dllmain.h"
#include "Hook/HookManager.h"
<<<<<<< HEAD
#include "Hook/Hooks_Diversion.h"
=======
>>>>>>> 2a08b0bfdc96c9e3f75df20f3f37bc8d8c09ee3a
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "OSTPlatform/include/DynamicLibrary.h"
<<<<<<< HEAD
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Thread.h"

#include <cstdio>
#include <windows.h>

// Resolve Steam paths before the client module is loaded so the diversion
// hook can redirect Steam's first LoadLibraryExW call.
static bool InitializeRuntimePaths()
{
    std::string steamPath = OSTPlatform::Encoding::WideToUtf8(
        OSTPlatform::DynamicLibrary::GetModuleDirectory(nullptr).wstring());
    if (steamPath.empty()) {
        steamPath = OSTPlatform::DynamicLibrary::GetCurrentDirectoryPath();
    }
    while (steamPath.size() > 1 && (steamPath.back() == '\\' || steamPath.back() == '/') &&
           !(steamPath.size() == 3 && steamPath[1] == ':')) {
        steamPath.pop_back();
    }
    if (steamPath.empty()) {
        return false;
    }

    const int baseLen = std::snprintf(SteamInstallPath, kRuntimePathCapacity, "%s", steamPath.c_str());
    const int clientLen = std::snprintf(SteamclientPath, kRuntimePathCapacity, "%s\\steamclient64.dll", steamPath.c_str());
    const int uiLen = std::snprintf(SteamUIPath, kRuntimePathCapacity, "%s\\steamui.dll", steamPath.c_str());
    const int diversionLen = std::snprintf(DiversionPath, kRuntimePathCapacity, "%s\\bin\\diversion64.dll", steamPath.c_str());
    const int luaLen = std::snprintf(LuaDir, kRuntimePathCapacity, "%s\\config\\lua", steamPath.c_str());
    const int configLen = std::snprintf(ConfigPath, kRuntimePathCapacity, "%s\\opensteamtool.toml", steamPath.c_str());
    return baseLen > 0 && static_cast<size_t>(baseLen) < kRuntimePathCapacity &&
           clientLen > 0 && static_cast<size_t>(clientLen) < kRuntimePathCapacity &&
           uiLen > 0 && static_cast<size_t>(uiLen) < kRuntimePathCapacity &&
           diversionLen > 0 && static_cast<size_t>(diversionLen) < kRuntimePathCapacity &&
           luaLen > 0 && static_cast<size_t>(luaLen) < kRuntimePathCapacity &&
           configLen > 0 && static_cast<size_t>(configLen) < kRuntimePathCapacity;
}

// Adopt the diverted module when available; otherwise retain the original
// steamclient64.dll loading path.
bool InitializeSteamComponents()
{
    auto client = client_hModule.load(std::memory_order_acquire);
    if (!client && g_IsDiversionActive.load(std::memory_order_acquire)) {
        auto loaded = OSTPlatform::DynamicLibrary::Load(std::filesystem::path(OSTPlatform::Encoding::Utf8ToWide(DiversionPath)));
        if (loaded) {
            auto expected = static_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(nullptr);
            client_hModule.compare_exchange_strong(expected, loaded, std::memory_order_acq_rel);
            client = client_hModule.load(std::memory_order_acquire);
            LOG_INFO("Loaded diversion module from {}", DiversionPath);
        }
    }
    if (!client) {
        auto loaded = OSTPlatform::DynamicLibrary::Load(std::filesystem::path(OSTPlatform::Encoding::Utf8ToWide(SteamclientPath)));
        if (!loaded) {
            LOG_ERROR("Load steamclient64.dll failed: {} (err={})",
                      SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
            return false;
        }
        auto expected = static_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(nullptr);
        client_hModule.compare_exchange_strong(expected, loaded, std::memory_order_acq_rel);
        client = client_hModule.load(std::memory_order_acquire);
        LOG_INFO("Loaded steamclient64.dll from {}", SteamclientPath);
    }

    auto ui = ui_hModule.load(std::memory_order_acquire);
    if (!ui) {
        ui = OSTPlatform::DynamicLibrary::Load(std::filesystem::path(OSTPlatform::Encoding::Utf8ToWide(SteamUIPath)));
        if (!ui) {
            LOG_ERROR("Load failed for steamui.dll: err={}", OSTPlatform::DynamicLibrary::GetLastErrorCode());
            return false;
        }
        ui_hModule.store(ui, std::memory_order_release);
=======
#include "OSTPlatform/include/Thread.h"

#include <windows.h>

// prepare key runtime paths.
bool InitializeSteamComponents()
{
    const std::string steamInstallPath = OSTPlatform::DynamicLibrary::GetCurrentDirectoryPath();
    if (steamInstallPath.empty()) {
        return false;
    }
    sprintf_s(SteamInstallPath, kRuntimePathCapacity, "%s", steamInstallPath.c_str());
    sprintf_s(SteamclientPath, kRuntimePathCapacity, "%s\\steamclient64.dll",  SteamInstallPath);
    sprintf_s(SteamUIPath,     kRuntimePathCapacity, "%s\\steamui.dll",        SteamInstallPath);
    sprintf_s(DiversionPath,   kRuntimePathCapacity, "%s\\bin\\diversion.dll", SteamInstallPath);
    sprintf_s(LuaDir,          kRuntimePathCapacity, "%s\\config\\lua",        SteamInstallPath);
    sprintf_s(ConfigPath,      kRuntimePathCapacity, "%s\\opensteamtool.toml", SteamInstallPath);
    
    client_hModule = OSTPlatform::DynamicLibrary::Load(SteamclientPath);
    if (!client_hModule) {
        LOG_ERROR("Load steamclient64.dll failed: {} (err={})",
                  SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    LOG_INFO("Loaded steamclient64.dll from {}", SteamclientPath);
    
    ui_hModule = OSTPlatform::DynamicLibrary::Load(SteamUIPath);
    if(!ui_hModule) {
        LOG_ERROR("Load failed for steamui.dll: err={}", OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
>>>>>>> 2a08b0bfdc96c9e3f75df20f3f37bc8d8c09ee3a
    }
    return true;
}

// All initialisation that touches the filesystem, loads modules, scans
// memory, or installs detours runs here on a worker thread — we MUST NOT do
// any of that from inside DllMain (loader lock).
static uint32_t InitThread(OSTPlatform::DynamicLibrary::ModuleHandle selfModule) {
    Log::Init(selfModule);
    LOG_INFO("OpenSteamTool init thread started");

    if (!InitializeSteamComponents()) {
        LOG_ERROR("InitializeSteamComponents failed");
        return 1;
    }

    Config::Load(ConfigPath);
    Log::InitModules();
    Log::InstallPlatformLogSink();
    SteamDiagnostics::Initialize(SteamclientPath, SteamUIPath);

    // Load pattern files for steamclient64.dll and steamui.dll.
    // Each call computes the SHA-256 of the DLL on disk, checks the local
    // cache, and downloads from GitHub if needed.  Both calls are synchronous
    // but run on this worker thread, never under the loader lock.
<<<<<<< HEAD
    PatternLoader::Load(ui_hModule.load(std::memory_order_acquire), SteamUIPath, "steamui");
    PatternLoader::Load(client_hModule.load(std::memory_order_acquire), SteamclientPath, "steamclient");
=======
    PatternLoader::Load(ui_hModule, SteamUIPath, "steamui");
    PatternLoader::Load(client_hModule, SteamclientPath, "steamclient");
>>>>>>> 2a08b0bfdc96c9e3f75df20f3f37bc8d8c09ee3a

    // IPC method metadata (funcHash, fencepost, argc, ...)
    IPCLoader::Load(SteamclientPath);

    std::vector<std::string> watchDirs = Config::GetLuaPaths();
    watchDirs.push_back(std::string(LuaDir));
    for (const auto& dir : watchDirs)
        LuaConfig::ParseDirectory(dir);

    LuaFileWatcher::Start(watchDirs);
    ConfigFileWatcher::Start(ConfigPath, LuaDir);

    SteamUI::CoreHook();
    SteamClient::CoreHook();
<<<<<<< HEAD
    g_HooksInstalled.store(true, std::memory_order_release);
    if (Hooks_Diversion::g_hClientHooksReadyEvent) {
        ::SetEvent(Hooks_Diversion::g_hClientHooksReadyEvent);
    }
=======
>>>>>>> 2a08b0bfdc96c9e3f75df20f3f37bc8d8c09ee3a

    // Surface any functions that FindPattern() could not locate.
    PatternLoader::ReportMissingFunctions();

    // Optional Steam Cloud save redirection (CloudRedirect). No-op unless
    // [cloud].enabled is set and cloud_redirect.dll is present.
    CloudRedirectHost::Initialize(SteamInstallPath);

    LOG_INFO("OpenSteamTool init complete");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
<<<<<<< HEAD
        auto selfModule = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hModule);

        // The paths and shadow client must be ready before Steam's first
        // steamclient64.dll load; the detour itself is installed synchronously.
        if (InitializeRuntimePaths()) {
            Hooks_Diversion::EnsureDiversionFileReady(
                OSTPlatform::Encoding::Utf8ToWide(SteamclientPath),
                OSTPlatform::Encoding::Utf8ToWide(DiversionPath));
            Hooks_Diversion::Install();
        }

        // Keep the DLL resident while process-wide detours and worker threads
        // can still call into it.
        HMODULE pinnedModule = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&DllMain), &pinnedModule);

        OSTPlatform::Thread::StartDetached([selfModule] {
            return InitThread(selfModule);
=======
        // Hand off all real work to a worker thread to avoid running file I/O,
        // module loading and detour transactions under the loader lock.
        OSTPlatform::Thread::StartDetached([module = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hModule)] {
            return InitThread(module);
>>>>>>> 2a08b0bfdc96c9e3f75df20f3f37bc8d8c09ee3a
        });
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
<<<<<<< HEAD
        g_HooksInstalled.store(false, std::memory_order_release);
        if (Hooks_Diversion::g_hClientHooksReadyEvent) {
            ::ResetEvent(Hooks_Diversion::g_hClientHooksReadyEvent);
        }
        if (pvReserved == nullptr) {
            Hooks_Diversion::Uninstall();
        }
=======
>>>>>>> 2a08b0bfdc96c9e3f75df20f3f37bc8d8c09ee3a
        ConfigFileWatcher::Stop();
        LuaFileWatcher::Stop();
        SteamUI::CoreUnhook();
        SteamClient::CoreUnhook();
        CloudRedirectHost::Shutdown();
    }

    return TRUE;
}
