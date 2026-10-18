#include "dllmain.h"
#include "Hook/HookManager.h"
#include "Hook/Hooks_Diversion.h"
#include "Hook/Hooks_SteamUI.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Thread.h"

#include <cstdio>
#include <cwchar>
#include <windows.h>

// Resolve Steam paths before the client module is loaded so the diversion
// hook can redirect Steam's first LoadLibraryExW call.
static bool InitializeRuntimePaths()
{
    const std::filesystem::path exePath = OSTPlatform::DynamicLibrary::GetMainExecutablePath();
    const std::wstring exeName = exePath.filename().wstring();
    if (exePath.empty() || _wcsicmp(exeName.c_str(), L"steam.exe") != 0)
    {
        return false;
    }

    std::string steamPath = OSTPlatform::Encoding::WideToUtf8(exePath.parent_path().wstring());
    while (steamPath.size() > 1 && (steamPath.back() == '\\' || steamPath.back() == '/') &&
           !(steamPath.size() == 3 && steamPath[1] == ':'))
    {
        steamPath.pop_back();
    }
    if (steamPath.empty())
    {
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

static bool InitializeSteamUI()
{
    auto ui = ui_hModule.load(std::memory_order_acquire);
    if (!ui)
    {
        ui = OSTPlatform::DynamicLibrary::Load(std::filesystem::path(OSTPlatform::Encoding::Utf8ToWide(SteamUIPath)));
        if (!ui)
        {
            LOG_ERROR("Load failed for steamui.dll: err={}", OSTPlatform::DynamicLibrary::GetLastErrorCode());
            return false;
        }
        ui_hModule.store(ui, std::memory_order_release);
    }
    return true;
}

static bool InitializeSteamClient()
{
    HMODULE client = Hooks_Diversion::EnsureClientModuleLoaded();
    if (!client)
    {
        LOG_ERROR("Load steamclient64.dll failed: {} (err={})",
                  SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    client_hModule.store(reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(client),
                         std::memory_order_release);

    if (g_IsDiversionActive.load(std::memory_order_acquire))
    {
        LOG_INFO("Loaded diversion steamclient64.dll from {}", DiversionPath);
    }
    else
    {
        LOG_WARN("Using original steamclient64.dll fallback from {}", SteamclientPath);
    }
    return true;
}

// All initialisation that touches the filesystem, loads modules, scans
// memory, or installs detours runs here on a worker thread — we MUST NOT do
// any of that from inside DllMain (loader lock).
static uint32_t InitThread(OSTPlatform::DynamicLibrary::ModuleHandle selfModule)
{
    Log::Init(selfModule);
    LOG_INFO("OpenSteamTool init thread started");

    // Keep file I/O off DllMain's loader lock. The early loader detour blocks
    // on this readiness event until the copy has been checked or failed.
    if (!Hooks_Diversion::EnsureDiversionFileReady(
            OSTPlatform::Encoding::Utf8ToWide(SteamclientPath),
            OSTPlatform::Encoding::Utf8ToWide(DiversionPath)))
    {
        LOG_WARN("Diversion copy unavailable; continuing with original steamclient fallback");
    }

    if (!InitializeSteamUI())
    {
        LOG_ERROR("InitializeSteamUI failed");
        return 1;
    }

    // This local .text signature scan is deliberately ahead of PatternLoader's
    // remote fetches so Steam's first client request cannot outrun the hook.
    if (!SteamUI::CoreLoaderHook())
    {
        LOG_WARN("SteamUI LoadModuleWithPath hook unavailable; retaining the system-loader fallback");
    }

    if (Hooks_Diversion::GetOriginalClientIfLoaded())
    {
        LOG_WARN("steamclient64.dll was already mapped before diversion adoption; selecting original fallback");
    }
    if (!InitializeSteamClient())
    {
        LOG_ERROR("InitializeSteamClient failed");
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
    PatternLoader::Load(ui_hModule.load(std::memory_order_acquire), SteamUIPath, "steamui");
    PatternLoader::Load(client_hModule.load(std::memory_order_acquire), SteamclientPath, "steamclient");

    // IPC method metadata (funcHash, fencepost, argc, ...)
    IPCLoader::Load(SteamclientPath);

    std::vector<std::string> watchDirs = Config::GetLuaPaths();
    watchDirs.push_back(std::string(LuaDir));
    for (const auto &dir : watchDirs)
        LuaConfig::ParseDirectory(dir);

    LuaFileWatcher::Start(watchDirs);
    ConfigFileWatcher::Start(ConfigPath, LuaDir);

    SteamUI::CoreHook();
    SteamClient::CoreHook();
    g_HooksInstalled.store(true, std::memory_order_release);
    if (Hooks_Diversion::g_hClientHooksReadyEvent)
    {
        ::SetEvent(Hooks_Diversion::g_hClientHooksReadyEvent);
    }

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
        auto selfModule = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hModule);

        // Only resolve paths and install loader detours here; the shadow-copy
        // check and module loads run on InitThread outside the loader lock.
        if (InitializeRuntimePaths())
        {
            Hooks_Diversion::ConfigurePaths(
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

        OSTPlatform::Thread::StartDetached([selfModule]
                                           { return InitThread(selfModule); });
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        g_HooksInstalled.store(false, std::memory_order_release);
        if (Hooks_Diversion::g_hClientHooksReadyEvent)
        {
            ::ResetEvent(Hooks_Diversion::g_hClientHooksReadyEvent);
        }
        if (pvReserved == nullptr)
        {
            Hooks_Diversion::Uninstall();
        }
        ConfigFileWatcher::Stop();
        LuaFileWatcher::Stop();
        SteamUI::CoreUnhook();
        SteamClient::CoreUnhook();
        CloudRedirectHost::Shutdown();
    }

    return TRUE;
}
