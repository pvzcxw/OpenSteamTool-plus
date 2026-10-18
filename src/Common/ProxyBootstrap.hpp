#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <string_view>

namespace OST::Proxy {

/**
 * @brief Defensive system DLL loader.
 * Loads original DLL directly from the Windows System32 directory using LOAD_WITH_ALTERED_SEARCH_PATH,
 * completely immune to local DLL sideloading and recursive self-loading.
 */
class SystemDllLoader {
public:
    [[nodiscard]] static HMODULE Load(std::wstring_view dllName) noexcept {
        if (dllName.empty()) {
            return nullptr;
        }

        wchar_t fullPath[MAX_PATH];
        const UINT len = ::GetSystemDirectoryW(fullPath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            return nullptr;
        }

        size_t pos = static_cast<size_t>(len);
        if (fullPath[pos - 1] != L'\\' && fullPath[pos - 1] != L'/') {
            fullPath[pos++] = L'\\';
        }

        if (dllName.size() >= MAX_PATH - pos) {
            return nullptr;
        }

        std::char_traits<wchar_t>::copy(fullPath + pos, dllName.data(), dllName.size());
        fullPath[pos + dllName.size()] = L'\0';

        return ::LoadLibraryExW(fullPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
};

/**
 * @brief Unified proxy DLL bootstrap and process filtering anchor.
 * Manages thread library call disabling, host process inspection, and guarded payload injection.
 */
/**
 * @brief Static ASCII ecosystem signature anchored into the binary image.
 * Third-party ecosystem tools (such as CloudRedirect's ThirdPartyDetector) scan proxy DLLs
 * (dwmapi.dll, xinput1_4.dll) for exact ASCII string markers ("OpenSteamTool") to recognize
 * compatible loaders and authorize automated deployment.
 */
inline constexpr char kEcosystemSignature[] = "OpenSteamTool";

class Bootstrap {
public:
    [[nodiscard]] static constexpr wchar_t ToLowerAscii(wchar_t ch) noexcept {
        return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch + (L'a' - L'A')) : ch;
    }

    [[nodiscard]] static constexpr bool EqualIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) return false;
        }
        return true;
    }

    static BOOL OnAttach(HMODULE hModule) noexcept {
        static volatile const char* volatile s_ecosystemSignature = kEcosystemSignature;
        (void)s_ecosystemSignature;

        ::DisableThreadLibraryCalls(hModule);
        static std::atomic<bool> workerStarted{false};
        if (workerStarted.exchange(true, std::memory_order_acq_rel)) return TRUE;

        HANDLE worker = ::CreateThread(nullptr, 0, &Bootstrap::DeferredLoad, hModule, 0, nullptr);
        if (worker) {
            ::CloseHandle(worker);
        } else {
            workerStarted.store(false, std::memory_order_release);
        }
        // Proxy loading succeeds even if the optional payload worker cannot start.
        return TRUE;
    }

private:
    static DWORD WINAPI DeferredLoad(LPVOID parameter) noexcept {
        ::Sleep(50);
        HMODULE hModule = static_cast<HMODULE>(parameter);
        if (!hModule) return 0;

        constexpr DWORD kPathCapacity = 1024;
        constexpr wchar_t kTargetHost[] = L"steam.exe";
        constexpr wchar_t kPayloadDll[] = L"OpenSteamTool.dll";
        wchar_t exePath[kPathCapacity]{};
        const DWORD exeLength = ::GetModuleFileNameW(nullptr, exePath, kPathCapacity);
        if (exeLength == 0 || exeLength >= kPathCapacity) return 0;

        const std::wstring_view fullExe(exePath, exeLength);
        const size_t exeSlash = fullExe.find_last_of(L"\\/");
        const std::wstring_view exeName = exeSlash == std::wstring_view::npos
            ? fullExe : fullExe.substr(exeSlash + 1);
        if (!EqualIgnoreCase(exeName, kTargetHost)) return 0;

        // A different bootstrap may already have loaded the payload.
        if (::GetModuleHandleW(kPayloadDll)) return 0;

        const size_t exeDirLength = exeSlash == std::wstring_view::npos ? 0 : exeSlash + 1;
        wchar_t proxyPath[kPathCapacity]{};
        const DWORD proxyLength = ::GetModuleFileNameW(hModule, proxyPath, kPathCapacity);
        size_t proxyDirLength = 0;
        if (proxyLength > 0 && proxyLength < kPathCapacity) {
            const std::wstring_view fullProxy(proxyPath, proxyLength);
            const size_t proxySlash = fullProxy.find_last_of(L"\\/");
            if (proxySlash != std::wstring_view::npos) proxyDirLength = proxySlash + 1;
        }

        wchar_t candidate[kPathCapacity]{};
        const auto loadFromDirectory = [&](const wchar_t* directory, size_t directoryLength) -> HMODULE {
            constexpr size_t payloadLength = std::size(kPayloadDll) - 1;
            if (directoryLength == 0 || directoryLength + payloadLength >= kPathCapacity) return nullptr;
            std::char_traits<wchar_t>::copy(candidate, directory, directoryLength);
            std::char_traits<wchar_t>::copy(candidate + directoryLength, kPayloadDll, payloadLength);
            candidate[directoryLength + payloadLength] = L'\0';
            return ::LoadLibraryExW(candidate, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        };

        HMODULE payload = loadFromDirectory(proxyPath, proxyDirLength);
        const bool sameDirectory = proxyDirLength == exeDirLength && proxyDirLength > 0 &&
            EqualIgnoreCase(std::wstring_view(proxyPath, proxyDirLength),
                            std::wstring_view(exePath, exeDirLength));
        if (!payload && !sameDirectory) payload = loadFromDirectory(exePath, exeDirLength);
        return payload ? 0 : 0;
    }
};
} // namespace OST::Proxy
