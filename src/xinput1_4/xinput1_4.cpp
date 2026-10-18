// xinput1_4.dll HiJack Project - True Dynamic Wrapper (With Undocumented Ordinals)
#include <windows.h>
#include <atomic>
#include <mutex>
#include "../Common/ProxyBootstrap.hpp"

// ─── 1. Real XInput Function Pointers ───────────────────────────
static std::atomic<HMODULE> g_hRealXInput{nullptr};
static std::mutex g_realXInputMutex;

// Standard
typedef DWORD(WINAPI* XInputGetState_t)(DWORD, void*);
typedef DWORD(WINAPI* XInputSetState_t)(DWORD, void*);
typedef DWORD(WINAPI* XInputGetCapabilities_t)(DWORD, DWORD, void*);
typedef void(WINAPI* XInputEnable_t)(BOOL);
typedef DWORD(WINAPI* XInputGetAudioDeviceIds_t)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*);
typedef DWORD(WINAPI* XInputGetBatteryInformation_t)(DWORD, BYTE, void*);
typedef DWORD(WINAPI* XInputGetKeystroke_t)(DWORD, DWORD, void*);

static XInputGetState_t o_XInputGetState = nullptr;
static XInputSetState_t o_XInputSetState = nullptr;
static XInputGetCapabilities_t o_XInputGetCapabilities = nullptr;
static XInputEnable_t o_XInputEnable = nullptr;
static XInputGetAudioDeviceIds_t o_XInputGetAudioDeviceIds = nullptr;
static XInputGetBatteryInformation_t o_XInputGetBatteryInformation = nullptr;
static XInputGetKeystroke_t o_XInputGetKeystroke = nullptr;

// Undocumented Ordinals (Required for Steam Big Picture / Guide Button)
static FARPROC o_100 = nullptr; // XInputGetStateEx
static FARPROC o_101 = nullptr; // XInputWaitForGuideButton
static FARPROC o_102 = nullptr; // XInputCancelGuideButtonWait
static FARPROC o_103 = nullptr; // XInputPowerOffController
static FARPROC o_104 = nullptr; // XInputGetBaseBusInformation
static FARPROC o_108 = nullptr; // XInputGetAudioDeviceIdsEx

// ─── 2. Core Initialization (Binding to the real System32 file) ───
void LoadRealXInput() {
    if (g_hRealXInput.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_realXInputMutex);
    if (g_hRealXInput.load(std::memory_order_relaxed)) return;

    // Resolve the system implementation by absolute path.  This avoids
    // recursively loading the proxy from Steam's directory and also avoids
    // relying on the process current directory.
    HMODULE realXInput = OST::Proxy::SystemDllLoader::Load(L"xinput1_4.dll");
    if (!realXInput) return; // Leave the state retryable on the next API call.

    // Load Standard API
    o_XInputGetState = (XInputGetState_t)GetProcAddress(realXInput, "XInputGetState");
    o_XInputSetState = (XInputSetState_t)GetProcAddress(realXInput, "XInputSetState");
    o_XInputGetCapabilities = (XInputGetCapabilities_t)GetProcAddress(realXInput, "XInputGetCapabilities");
    o_XInputEnable = (XInputEnable_t)GetProcAddress(realXInput, "XInputEnable");
    o_XInputGetAudioDeviceIds = (XInputGetAudioDeviceIds_t)GetProcAddress(realXInput, "XInputGetAudioDeviceIds");
    o_XInputGetBatteryInformation = (XInputGetBatteryInformation_t)GetProcAddress(realXInput, "XInputGetBatteryInformation");
    o_XInputGetKeystroke = (XInputGetKeystroke_t)GetProcAddress(realXInput, "XInputGetKeystroke");

    // Load Undocumented Ordinals
    o_100 = GetProcAddress(realXInput, (LPCSTR)100);
    o_101 = GetProcAddress(realXInput, (LPCSTR)101);
    o_102 = GetProcAddress(realXInput, (LPCSTR)102);
    o_103 = GetProcAddress(realXInput, (LPCSTR)103);
    o_104 = GetProcAddress(realXInput, (LPCSTR)104);
    o_108 = GetProcAddress(realXInput, (LPCSTR)108);

    // Publish last: callers that observe the handle with acquire semantics see
    // the complete dispatch table above.
    g_hRealXInput.store(realXInput, std::memory_order_release);
}

// ─── 3. Native Exports (Safely passing data to the game) ──────────
extern "C" {
    // Standard Functions
    DWORD WINAPI XInputGetState(DWORD dwUserIndex, void* pState) {
        LoadRealXInput();
        return o_XInputGetState ? o_XInputGetState(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputSetState(DWORD dwUserIndex, void* pVibration) {
        LoadRealXInput();
        return o_XInputSetState ? o_XInputSetState(dwUserIndex, pVibration) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags, void* pCapabilities) {
        LoadRealXInput();
        return o_XInputGetCapabilities ? o_XInputGetCapabilities(dwUserIndex, dwFlags, pCapabilities) : ERROR_DEVICE_NOT_CONNECTED;
    }
    void WINAPI XInputEnable(BOOL enable) {
        LoadRealXInput();
        if (o_XInputEnable) o_XInputEnable(enable);
    }
    DWORD WINAPI XInputGetAudioDeviceIds(DWORD dwUserIndex, LPWSTR pRenderDeviceId, UINT* pRenderCount, LPWSTR pCaptureDeviceId, UINT* pCaptureCount) {
        LoadRealXInput();
        return o_XInputGetAudioDeviceIds ? o_XInputGetAudioDeviceIds(dwUserIndex, pRenderDeviceId, pRenderCount, pCaptureDeviceId, pCaptureCount) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType, void* pBatteryInformation) {
        LoadRealXInput();
        return o_XInputGetBatteryInformation ? o_XInputGetBatteryInformation(dwUserIndex, devType, pBatteryInformation) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved, void* pKeystroke) {
        LoadRealXInput();
        return o_XInputGetKeystroke ? o_XInputGetKeystroke(dwUserIndex, dwReserved, pKeystroke) : ERROR_DEVICE_NOT_CONNECTED;
    }

    // Undocumented Ordinal Wrappers
    DWORD WINAPI XInputOrdinal100(DWORD a1, void* a2) {
        LoadRealXInput();
        return o_100 ? ((DWORD(WINAPI*)(DWORD, void*))o_100)(a1, a2) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputOrdinal101(DWORD a1, DWORD a2, void* a3) {
        LoadRealXInput();
        return o_101 ? ((DWORD(WINAPI*)(DWORD, DWORD, void*))o_101)(a1, a2, a3) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputOrdinal102(DWORD a1) {
        LoadRealXInput();
        return o_102 ? ((DWORD(WINAPI*)(DWORD))o_102)(a1) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputOrdinal103(DWORD a1) {
        LoadRealXInput();
        return o_103 ? ((DWORD(WINAPI*)(DWORD))o_103)(a1) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputOrdinal104(DWORD a1, void* a2) {
        LoadRealXInput();
        return o_104 ? ((DWORD(WINAPI*)(DWORD, void*))o_104)(a1, a2) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputOrdinal108(DWORD a1, void* a2, void* a3, void* a4, void* a5) {
        LoadRealXInput();
        return o_108 ? ((DWORD(WINAPI*)(DWORD, void*, void*, void*, void*))o_108)(a1, a2, a3, a4, a5) : ERROR_DEVICE_NOT_CONNECTED;
    }
}

// ─── 4. OpenSteamTool Injection ───────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID /*pvReserved*/) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        // The proxy is intentionally inert in DllMain: Bootstrap performs a
        // delayed, absolute-path payload load on a worker thread after the
        // loader lock is released.  XInput itself is bound lazily on first use.
        return OST::Proxy::Bootstrap::OnAttach(hModule);
    }
    return TRUE;
}
