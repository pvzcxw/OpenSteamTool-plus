#pragma once

#include <string>
#include <windows.h>

namespace Hooks_Diversion {
    // Synchronization event signaled when InitThread completes InstallClientHooks.
    // Prevents steam.exe from invoking diversion exports before hooks are active.
    inline HANDLE g_hClientHooksReadyEvent = nullptr;

    // Ensures diversion shadow file (bin\diversion64.dll) is up to date with steamclient64.dll.
    // Performs microsecond-level metadata check (size + mtime).
    bool EnsureDiversionFileReady(const std::wstring& originalPath, const std::wstring& diversionPath) noexcept;

    // Installs the chokepoint detours on KERNELBASE LoadLibraryExW & GetModuleHandle family.
    void Install() noexcept;

    // Uninstalls the chokepoint detours.
    void Uninstall() noexcept;

    // Returns whether the detour is currently active.
    [[nodiscard]] bool IsInstalled() noexcept;
}
