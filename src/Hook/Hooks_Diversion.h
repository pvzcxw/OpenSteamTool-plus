#pragma once

#include <string>
#include <windows.h>

namespace Hooks_Diversion {
    // Synchronization event signaled when InitThread completes InstallClientHooks.
    // Prevents steam.exe from invoking diversion exports before hooks are active.
    inline HANDLE g_hClientHooksReadyEvent = nullptr;
    inline HANDLE g_hDiversionFileReadyEvent = nullptr;

    // Set before the loader detours are installed; filesystem work happens on InitThread.
    void ConfigurePaths(const std::wstring& originalPath, const std::wstring& diversionPath) noexcept;

    // Ensures diversion shadow file (bin\diversion64.dll) is up to date with steamclient64.dll.
    // Performs microsecond-level metadata check (size + mtime).
    bool EnsureDiversionFileReady(const std::wstring& originalPath, const std::wstring& diversionPath) noexcept;

    // Loads the shadow module (or the already-loaded/original client fallback) once.
    HMODULE EnsureClientModuleLoaded() noexcept;

    // Obtains an additional loader reference suitable for returning to a caller
    // that expects LoadModuleWithPath-style ownership semantics.
    HMODULE AcquireClientModuleReference() noexcept;

    // Returns the original client module through the unhooked loader entrypoint.
    // This is used by the SteamUI loader hook to detect a client that won the
    // race before the diversion path was installed.
    HMODULE GetOriginalClientIfLoaded() noexcept;

    // Installs the chokepoint detours on KERNELBASE LoadLibraryExW & GetModuleHandle family.
    void Install() noexcept;

    // Uninstalls the chokepoint detours.
    void Uninstall() noexcept;

    // Returns whether the detour is currently active.
    [[nodiscard]] bool IsInstalled() noexcept;
}
