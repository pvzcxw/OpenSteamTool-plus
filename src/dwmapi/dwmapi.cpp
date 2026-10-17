<<<<<<< HEAD
// dwmapi.dll HiJack Project - Dynamic System Wrapper using ProxyBootstrap
#include <windows.h>
#include <atomic>
#include <mutex>
#include "../Common/ProxyBootstrap.hpp"

// ─── 1. Core State & System Loader ──────────────────────────────────────────
static std::once_flag g_dwmapiInitOnce;
static std::atomic<bool> g_dwmapiReady{false};
static HMODULE g_hRealDwmapi = nullptr;

// ─── 2. Typed Pointers for Public Core DWM APIs (Steam/VGUI High-Frequency) ──
using DwmFlush_t = HRESULT(WINAPI*)(void);
using DwmIsCompositionEnabled_t = HRESULT(WINAPI*)(BOOL*);
using DwmGetWindowAttribute_t = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
using DwmSetWindowAttribute_t = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
using DwmExtendFrameIntoClientArea_t = HRESULT(WINAPI*)(HWND, const void*);
using DwmEnableBlurBehindWindow_t = HRESULT(WINAPI*)(HWND, const void*);
using DwmDefWindowProc_t = BOOL(WINAPI*)(HWND, UINT, WPARAM, LPARAM, LRESULT*);
using DwmGetColorizationColor_t = HRESULT(WINAPI*)(DWORD*, BOOL*);

using DllCanUnloadNow_t = HRESULT(WINAPI*)(void);
using DllGetClassObject_t = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

static DwmFlush_t g_pfnDwmFlush = nullptr;
static DwmIsCompositionEnabled_t g_pfnDwmIsCompositionEnabled = nullptr;
static DwmGetWindowAttribute_t g_pfnDwmGetWindowAttribute = nullptr;
static DwmSetWindowAttribute_t g_pfnDwmSetWindowAttribute = nullptr;
static DwmExtendFrameIntoClientArea_t g_pfnDwmExtendFrameIntoClientArea = nullptr;
static DwmEnableBlurBehindWindow_t g_pfnDwmEnableBlurBehindWindow = nullptr;
static DwmDefWindowProc_t g_pfnDwmDefWindowProc = nullptr;
static DwmGetColorizationColor_t g_pfnDwmGetColorizationColor = nullptr;

static DllCanUnloadNow_t g_pfnDllCanUnloadNow = nullptr;
static DllGetClassObject_t g_pfnDllGetClassObject = nullptr;

// ─── 3. Flat Dispatch Table for Undocumented Ordinals & Secondary Exports ───
static FARPROC g_pfnOrdinals[256] = {};

#define DECLARE_NAMED_POINTER(name) static FARPROC g_pfn_##name = nullptr;

DECLARE_NAMED_POINTER(DwmAttachMilContent)
DECLARE_NAMED_POINTER(DwmDetachMilContent)
DECLARE_NAMED_POINTER(DwmEnableComposition)
DECLARE_NAMED_POINTER(DwmEnableMMCSS)
DECLARE_NAMED_POINTER(DwmGetCompositionTimingInfo)
DECLARE_NAMED_POINTER(DwmGetGraphicsStreamClient)
DECLARE_NAMED_POINTER(DwmGetGraphicsStreamTransformHint)
DECLARE_NAMED_POINTER(DwmGetTransportAttributes)
DECLARE_NAMED_POINTER(DwmGetUnmetTabRequirements)
DECLARE_NAMED_POINTER(DwmInvalidateIconicBitmaps)
DECLARE_NAMED_POINTER(DwmModifyPreviousDxFrameDuration)
DECLARE_NAMED_POINTER(DwmQueryThumbnailSourceSize)
DECLARE_NAMED_POINTER(DwmRegisterThumbnail)
DECLARE_NAMED_POINTER(DwmRenderGesture)
DECLARE_NAMED_POINTER(DwmSetDxFrameDuration)
DECLARE_NAMED_POINTER(DwmSetIconicLivePreviewBitmap)
DECLARE_NAMED_POINTER(DwmSetIconicThumbnail)
DECLARE_NAMED_POINTER(DwmSetPresentParameters)
DECLARE_NAMED_POINTER(DwmShowContact)
DECLARE_NAMED_POINTER(DwmTetherContact)
DECLARE_NAMED_POINTER(DwmTetherTextContact)
DECLARE_NAMED_POINTER(DwmTransitionOwnedWindow)
DECLARE_NAMED_POINTER(DwmUnregisterThumbnail)
DECLARE_NAMED_POINTER(DwmUpdateThumbnailProperties)
DECLARE_NAMED_POINTER(DwmpAllocateSecurityDescriptor)
DECLARE_NAMED_POINTER(DwmpDxGetWindowSharedSurface)
DECLARE_NAMED_POINTER(DwmpDxUpdateWindowSharedSurface)
DECLARE_NAMED_POINTER(DwmpDxgiIsThreadDesktopComposited)
DECLARE_NAMED_POINTER(DwmpEnableDDASupport)
DECLARE_NAMED_POINTER(DwmpFreeSecurityDescriptor)
DECLARE_NAMED_POINTER(DwmpGetColorizationParameters)
DECLARE_NAMED_POINTER(DwmpRenderFlick)
DECLARE_NAMED_POINTER(DwmpSetColorizationParameters)
DECLARE_NAMED_POINTER(DwmpUpdateProxyWindowForCapture)

#undef DECLARE_NAMED_POINTER

// ─── 4. Pre-Binding on Initialization (Zero-Lock Dispatch) ──────────────────
static void LoadRealDwmapi() noexcept {
    std::call_once(g_dwmapiInitOnce, []() noexcept {
        g_hRealDwmapi = OST::Proxy::SystemDllLoader::Load(L"dwmapi.dll");
        if (g_hRealDwmapi) {
            // Core Typed APIs
            g_pfnDwmFlush = reinterpret_cast<DwmFlush_t>(GetProcAddress(g_hRealDwmapi, "DwmFlush"));
            g_pfnDwmIsCompositionEnabled = reinterpret_cast<DwmIsCompositionEnabled_t>(GetProcAddress(g_hRealDwmapi, "DwmIsCompositionEnabled"));
            g_pfnDwmGetWindowAttribute = reinterpret_cast<DwmGetWindowAttribute_t>(GetProcAddress(g_hRealDwmapi, "DwmGetWindowAttribute"));
            g_pfnDwmSetWindowAttribute = reinterpret_cast<DwmSetWindowAttribute_t>(GetProcAddress(g_hRealDwmapi, "DwmSetWindowAttribute"));
            g_pfnDwmExtendFrameIntoClientArea = reinterpret_cast<DwmExtendFrameIntoClientArea_t>(GetProcAddress(g_hRealDwmapi, "DwmExtendFrameIntoClientArea"));
            g_pfnDwmEnableBlurBehindWindow = reinterpret_cast<DwmEnableBlurBehindWindow_t>(GetProcAddress(g_hRealDwmapi, "DwmEnableBlurBehindWindow"));
            g_pfnDwmDefWindowProc = reinterpret_cast<DwmDefWindowProc_t>(GetProcAddress(g_hRealDwmapi, "DwmDefWindowProc"));
            g_pfnDwmGetColorizationColor = reinterpret_cast<DwmGetColorizationColor_t>(GetProcAddress(g_hRealDwmapi, "DwmGetColorizationColor"));

            g_pfnDllCanUnloadNow = reinterpret_cast<DllCanUnloadNow_t>(GetProcAddress(g_hRealDwmapi, "DllCanUnloadNow"));
            g_pfnDllGetClassObject = reinterpret_cast<DllGetClassObject_t>(GetProcAddress(g_hRealDwmapi, "DllGetClassObject"));

            // Secondary Named Exports
            #define BIND_NAMED(name) g_pfn_##name = GetProcAddress(g_hRealDwmapi, #name);
            BIND_NAMED(DwmAttachMilContent)
            BIND_NAMED(DwmDetachMilContent)
            BIND_NAMED(DwmEnableComposition)
            BIND_NAMED(DwmEnableMMCSS)
            BIND_NAMED(DwmGetCompositionTimingInfo)
            BIND_NAMED(DwmGetGraphicsStreamClient)
            BIND_NAMED(DwmGetGraphicsStreamTransformHint)
            BIND_NAMED(DwmGetTransportAttributes)
            BIND_NAMED(DwmGetUnmetTabRequirements)
            BIND_NAMED(DwmInvalidateIconicBitmaps)
            BIND_NAMED(DwmModifyPreviousDxFrameDuration)
            BIND_NAMED(DwmQueryThumbnailSourceSize)
            BIND_NAMED(DwmRegisterThumbnail)
            BIND_NAMED(DwmRenderGesture)
            BIND_NAMED(DwmSetDxFrameDuration)
            BIND_NAMED(DwmSetIconicLivePreviewBitmap)
            BIND_NAMED(DwmSetIconicThumbnail)
            BIND_NAMED(DwmSetPresentParameters)
            BIND_NAMED(DwmShowContact)
            BIND_NAMED(DwmTetherContact)
            BIND_NAMED(DwmTetherTextContact)
            BIND_NAMED(DwmTransitionOwnedWindow)
            BIND_NAMED(DwmUnregisterThumbnail)
            BIND_NAMED(DwmUpdateThumbnailProperties)
            BIND_NAMED(DwmpAllocateSecurityDescriptor)
            BIND_NAMED(DwmpDxGetWindowSharedSurface)
            BIND_NAMED(DwmpDxUpdateWindowSharedSurface)
            BIND_NAMED(DwmpDxgiIsThreadDesktopComposited)
            BIND_NAMED(DwmpEnableDDASupport)
            BIND_NAMED(DwmpFreeSecurityDescriptor)
            BIND_NAMED(DwmpGetColorizationParameters)
            BIND_NAMED(DwmpRenderFlick)
            BIND_NAMED(DwmpSetColorizationParameters)
            BIND_NAMED(DwmpUpdateProxyWindowForCapture)
            #undef BIND_NAMED

            // Ordinal Table (103 ~ 204)
            constexpr int kKnownOrdinals[] = {
                103, 104, 105, 106, 107, 108, 109, 110,
                112, 113, 114,
                124,
                132,
                138, 139, 140, 141, 142,
                144, 145, 146,
                147, 148,
                150, 151, 152, 153, 154, 155,
                157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172,
                173, 174, 175, 176, 177, 178, 179, 180, 181, 182,
                184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197,
                199, 200, 201, 202, 203, 204
            };
            for (int ord : kKnownOrdinals) {
                if (ord < 256) {
                    g_pfnOrdinals[ord] = GetProcAddress(g_hRealDwmapi, MAKEINTRESOURCEA(ord));
                }
            }
        }
        g_dwmapiReady.store(true, std::memory_order_release);
    });
}

inline void EnsureRealDwmapiLoaded() noexcept {
    if (!g_dwmapiReady.load(std::memory_order_acquire)) {
        LoadRealDwmapi();
    }
}

// ─── 5. Export Stubs ────────────────────────────────────────────────────────
#define FORWARD_NAMED(name) \
    FARPROC WINAPI name(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) { \
        EnsureRealDwmapiLoaded(); \
        const auto pfn = reinterpret_cast<FARPROC(WINAPI*)(void*, void*, void*, void*, void*, void*, void*, void*)>(g_pfn_##name); \
        return pfn ? pfn(a1, a2, a3, a4, a5, a6, a7, a8) : nullptr; \
    }

#define FORWARD_ORDINAL(stub, ord) \
    FARPROC WINAPI stub(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) { \
        EnsureRealDwmapiLoaded(); \
        const auto pfn = reinterpret_cast<FARPROC(WINAPI*)(void*, void*, void*, void*, void*, void*, void*, void*)>(g_pfnOrdinals[ord]); \
        return pfn ? pfn(a1, a2, a3, a4, a5, a6, a7, a8) : nullptr; \
    }

extern "C" {

HRESULT WINAPI DllCanUnloadNow(void) {
    EnsureRealDwmapiLoaded();
    return g_pfnDllCanUnloadNow ? g_pfnDllCanUnloadNow() : S_OK;
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    EnsureRealDwmapiLoaded();
    return g_pfnDllGetClassObject ? g_pfnDllGetClassObject(rclsid, riid, ppv) : CLASS_E_CLASSNOTAVAILABLE;
}

// Core Strongly-Typed Functions
HRESULT WINAPI DwmFlush(void) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmFlush ? g_pfnDwmFlush() : S_OK;
}

HRESULT WINAPI DwmIsCompositionEnabled(BOOL* pfEnabled) {
    EnsureRealDwmapiLoaded();
    if (!g_pfnDwmIsCompositionEnabled) {
        if (pfEnabled) *pfEnabled = FALSE;
        return S_OK;
    }
    return g_pfnDwmIsCompositionEnabled(pfEnabled);
}

HRESULT WINAPI DwmGetWindowAttribute(HWND hwnd, DWORD dwAttribute, PVOID pvAttribute, DWORD cbAttribute) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmGetWindowAttribute ? g_pfnDwmGetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute) : E_NOTIMPL;
}

HRESULT WINAPI DwmSetWindowAttribute(HWND hwnd, DWORD dwAttribute, LPCVOID pvAttribute, DWORD cbAttribute) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmSetWindowAttribute ? g_pfnDwmSetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute) : S_OK;
}

HRESULT WINAPI DwmExtendFrameIntoClientArea(HWND hWnd, const void* pMarInset) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmExtendFrameIntoClientArea ? g_pfnDwmExtendFrameIntoClientArea(hWnd, pMarInset) : S_OK;
}

HRESULT WINAPI DwmEnableBlurBehindWindow(HWND hWnd, const void* pBlurBehind) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmEnableBlurBehindWindow ? g_pfnDwmEnableBlurBehindWindow(hWnd, pBlurBehind) : S_OK;
}

BOOL WINAPI DwmDefWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* plResult) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmDefWindowProc ? g_pfnDwmDefWindowProc(hWnd, msg, wParam, lParam, plResult) : FALSE;
}

HRESULT WINAPI DwmGetColorizationColor(DWORD* pcrColorization, BOOL* pfOpaqueBlend) {
    EnsureRealDwmapiLoaded();
    if (!g_pfnDwmGetColorizationColor) {
        if (pcrColorization) *pcrColorization = 0;
        if (pfOpaqueBlend) *pfOpaqueBlend = FALSE;
        return S_OK;
    }
    return g_pfnDwmGetColorizationColor(pcrColorization, pfOpaqueBlend);
}

// Secondary Named Exports
FORWARD_NAMED(DwmAttachMilContent)
FORWARD_NAMED(DwmDetachMilContent)
FORWARD_NAMED(DwmEnableComposition)
FORWARD_NAMED(DwmEnableMMCSS)
FORWARD_NAMED(DwmGetCompositionTimingInfo)
FORWARD_NAMED(DwmGetGraphicsStreamClient)
FORWARD_NAMED(DwmGetGraphicsStreamTransformHint)
FORWARD_NAMED(DwmGetTransportAttributes)
FORWARD_NAMED(DwmGetUnmetTabRequirements)
FORWARD_NAMED(DwmInvalidateIconicBitmaps)
FORWARD_NAMED(DwmModifyPreviousDxFrameDuration)
FORWARD_NAMED(DwmQueryThumbnailSourceSize)
FORWARD_NAMED(DwmRegisterThumbnail)
FORWARD_NAMED(DwmRenderGesture)
FORWARD_NAMED(DwmSetDxFrameDuration)
FORWARD_NAMED(DwmSetIconicLivePreviewBitmap)
FORWARD_NAMED(DwmSetIconicThumbnail)
FORWARD_NAMED(DwmSetPresentParameters)
FORWARD_NAMED(DwmShowContact)
FORWARD_NAMED(DwmTetherContact)
FORWARD_NAMED(DwmTetherTextContact)
FORWARD_NAMED(DwmTransitionOwnedWindow)
FORWARD_NAMED(DwmUnregisterThumbnail)
FORWARD_NAMED(DwmUpdateThumbnailProperties)
FORWARD_NAMED(DwmpAllocateSecurityDescriptor)
FORWARD_NAMED(DwmpDxGetWindowSharedSurface)
FORWARD_NAMED(DwmpDxUpdateWindowSharedSurface)
FORWARD_NAMED(DwmpDxgiIsThreadDesktopComposited)
FORWARD_NAMED(DwmpEnableDDASupport)
FORWARD_NAMED(DwmpFreeSecurityDescriptor)
FORWARD_NAMED(DwmpGetColorizationParameters)
FORWARD_NAMED(DwmpRenderFlick)
FORWARD_NAMED(DwmpSetColorizationParameters)
FORWARD_NAMED(DwmpUpdateProxyWindowForCapture)

// Ordinals
FORWARD_ORDINAL(DwmpOrdinal103, 103)
FORWARD_ORDINAL(DwmpOrdinal104, 104)
FORWARD_ORDINAL(DwmpOrdinal105, 105)
FORWARD_ORDINAL(DwmpOrdinal106, 106)
FORWARD_ORDINAL(DwmpOrdinal107, 107)
FORWARD_ORDINAL(DwmpOrdinal108, 108)
FORWARD_ORDINAL(DwmpOrdinal109, 109)
FORWARD_ORDINAL(DwmpOrdinal110, 110)
FORWARD_ORDINAL(DwmpOrdinal112, 112)
FORWARD_ORDINAL(DwmpOrdinal113, 113)
FORWARD_ORDINAL(DwmpOrdinal114, 114)
FORWARD_ORDINAL(DwmpOrdinal124, 124)
FORWARD_ORDINAL(DwmpOrdinal132, 132)
FORWARD_ORDINAL(DwmpOrdinal138, 138)
FORWARD_ORDINAL(DwmpOrdinal139, 139)
FORWARD_ORDINAL(DwmpOrdinal140, 140)
FORWARD_ORDINAL(DwmpOrdinal141, 141)
FORWARD_ORDINAL(DwmpOrdinal142, 142)
FORWARD_ORDINAL(DwmpOrdinal144, 144)
FORWARD_ORDINAL(DwmpOrdinal145, 145)
FORWARD_ORDINAL(DwmpOrdinal146, 146)
FORWARD_ORDINAL(DwmpOrdinal147, 147)
FORWARD_ORDINAL(DwmpOrdinal148, 148)
FORWARD_ORDINAL(DwmpOrdinal150, 150)
FORWARD_ORDINAL(DwmpOrdinal151, 151)
FORWARD_ORDINAL(DwmpOrdinal152, 152)
FORWARD_ORDINAL(DwmpOrdinal153, 153)
FORWARD_ORDINAL(DwmpOrdinal154, 154)
FORWARD_ORDINAL(DwmpOrdinal155, 155)
FORWARD_ORDINAL(DwmpOrdinal157, 157)
FORWARD_ORDINAL(DwmpOrdinal158, 158)
FORWARD_ORDINAL(DwmpOrdinal159, 159)
FORWARD_ORDINAL(DwmpOrdinal160, 160)
FORWARD_ORDINAL(DwmpOrdinal161, 161)
FORWARD_ORDINAL(DwmpOrdinal162, 162)
FORWARD_ORDINAL(DwmpOrdinal163, 163)
FORWARD_ORDINAL(DwmpOrdinal164, 164)
FORWARD_ORDINAL(DwmpOrdinal165, 165)
FORWARD_ORDINAL(DwmpOrdinal166, 166)
FORWARD_ORDINAL(DwmpOrdinal167, 167)
FORWARD_ORDINAL(DwmpOrdinal168, 168)
FORWARD_ORDINAL(DwmpOrdinal169, 169)
FORWARD_ORDINAL(DwmpOrdinal170, 170)
FORWARD_ORDINAL(DwmpOrdinal171, 171)
FORWARD_ORDINAL(DwmpOrdinal172, 172)
FORWARD_ORDINAL(DwmpOrdinal173, 173)
FORWARD_ORDINAL(DwmpOrdinal174, 174)
FORWARD_ORDINAL(DwmpOrdinal175, 175)
FORWARD_ORDINAL(DwmpOrdinal176, 176)
FORWARD_ORDINAL(DwmpOrdinal177, 177)
FORWARD_ORDINAL(DwmpOrdinal178, 178)
FORWARD_ORDINAL(DwmpOrdinal179, 179)
FORWARD_ORDINAL(DwmpOrdinal180, 180)
FORWARD_ORDINAL(DwmpOrdinal181, 181)
FORWARD_ORDINAL(DwmpOrdinal182, 182)
FORWARD_ORDINAL(DwmpOrdinal184, 184)
FORWARD_ORDINAL(DwmpOrdinal185, 185)
FORWARD_ORDINAL(DwmpOrdinal186, 186)
FORWARD_ORDINAL(DwmpOrdinal187, 187)
FORWARD_ORDINAL(DwmpOrdinal188, 188)
FORWARD_ORDINAL(DwmpOrdinal189, 189)
FORWARD_ORDINAL(DwmpOrdinal190, 190)
FORWARD_ORDINAL(DwmpOrdinal191, 191)
FORWARD_ORDINAL(DwmpOrdinal192, 192)
FORWARD_ORDINAL(DwmpOrdinal193, 193)
FORWARD_ORDINAL(DwmpOrdinal194, 194)
FORWARD_ORDINAL(DwmpOrdinal195, 195)
FORWARD_ORDINAL(DwmpOrdinal196, 196)
FORWARD_ORDINAL(DwmpOrdinal197, 197)
FORWARD_ORDINAL(DwmpOrdinal199, 199)
FORWARD_ORDINAL(DwmpOrdinal200, 200)
FORWARD_ORDINAL(DwmpOrdinal201, 201)
FORWARD_ORDINAL(DwmpOrdinal202, 202)
FORWARD_ORDINAL(DwmpOrdinal203, 203)
FORWARD_ORDINAL(DwmpOrdinal204, 204)

} // extern "C"

#undef FORWARD_NAMED
#undef FORWARD_ORDINAL

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID /*pvReserved*/) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        return OST::Proxy::Bootstrap::OnAttach(hModule);
    }
    return TRUE;
}
=======
// dwmapi.dll HiJack Project

#include <windows.h>
#include <cstring>


#pragma comment(linker, "/EXPORT:DllCanUnloadNow=DWMAPI.DllCanUnloadNow,@111")
#pragma comment(linker, "/EXPORT:DllGetClassObject=DWMAPI.DllGetClassObject,@115")
#pragma comment(linker, "/EXPORT:DwmAttachMilContent=DWMAPI.DwmAttachMilContent,@116")
#pragma comment(linker, "/EXPORT:DwmDefWindowProc=DWMAPI.DwmDefWindowProc,@117")
#pragma comment(linker, "/EXPORT:DwmDetachMilContent=DWMAPI.DwmDetachMilContent,@118")
#pragma comment(linker, "/EXPORT:DwmEnableBlurBehindWindow=DWMAPI.DwmEnableBlurBehindWindow,@119")
#pragma comment(linker, "/EXPORT:DwmEnableComposition=DWMAPI.DwmEnableComposition,@102")
#pragma comment(linker, "/EXPORT:DwmEnableMMCSS=DWMAPI.DwmEnableMMCSS,@120")
#pragma comment(linker, "/EXPORT:DwmExtendFrameIntoClientArea=DWMAPI.DwmExtendFrameIntoClientArea,@121")
#pragma comment(linker, "/EXPORT:DwmFlush=DWMAPI.DwmFlush,@122")
#pragma comment(linker, "/EXPORT:DwmGetColorizationColor=DWMAPI.DwmGetColorizationColor,@123")
#pragma comment(linker, "/EXPORT:DwmGetCompositionTimingInfo=DWMAPI.DwmGetCompositionTimingInfo,@125")
#pragma comment(linker, "/EXPORT:DwmGetGraphicsStreamClient=DWMAPI.DwmGetGraphicsStreamClient,@126")
#pragma comment(linker, "/EXPORT:DwmGetGraphicsStreamTransformHint=DWMAPI.DwmGetGraphicsStreamTransformHint,@129")
#pragma comment(linker, "/EXPORT:DwmGetTransportAttributes=DWMAPI.DwmGetTransportAttributes,@130")
#pragma comment(linker, "/EXPORT:DwmGetUnmetTabRequirements=DWMAPI.DwmGetUnmetTabRequirements,@133")
#pragma comment(linker, "/EXPORT:DwmGetWindowAttribute=DWMAPI.DwmGetWindowAttribute,@134")
#pragma comment(linker, "/EXPORT:DwmInvalidateIconicBitmaps=DWMAPI.DwmInvalidateIconicBitmaps,@149")
#pragma comment(linker, "/EXPORT:DwmIsCompositionEnabled=DWMAPI.DwmIsCompositionEnabled,@188")
#pragma comment(linker, "/EXPORT:DwmModifyPreviousDxFrameDuration=DWMAPI.DwmModifyPreviousDxFrameDuration,@189")
#pragma comment(linker, "/EXPORT:DwmQueryThumbnailSourceSize=DWMAPI.DwmQueryThumbnailSourceSize,@190")
#pragma comment(linker, "/EXPORT:DwmRegisterThumbnail=DWMAPI.DwmRegisterThumbnail,@191")
#pragma comment(linker, "/EXPORT:DwmRenderGesture=DWMAPI.DwmRenderGesture,@192")
#pragma comment(linker, "/EXPORT:DwmSetDxFrameDuration=DWMAPI.DwmSetDxFrameDuration,@193")
#pragma comment(linker, "/EXPORT:DwmSetIconicLivePreviewBitmap=DWMAPI.DwmSetIconicLivePreviewBitmap,@194")
#pragma comment(linker, "/EXPORT:DwmSetIconicThumbnail=DWMAPI.DwmSetIconicThumbnail,@195")
#pragma comment(linker, "/EXPORT:DwmSetPresentParameters=DWMAPI.DwmSetPresentParameters,@196")
#pragma comment(linker, "/EXPORT:DwmSetWindowAttribute=DWMAPI.DwmSetWindowAttribute,@197")
#pragma comment(linker, "/EXPORT:DwmShowContact=DWMAPI.DwmShowContact,@198")
#pragma comment(linker, "/EXPORT:DwmTetherContact=DWMAPI.DwmTetherContact,@199")
#pragma comment(linker, "/EXPORT:DwmTetherTextContact=DWMAPI.DwmTetherTextContact,@156")
#pragma comment(linker, "/EXPORT:DwmTransitionOwnedWindow=DWMAPI.DwmTransitionOwnedWindow,@200")
#pragma comment(linker, "/EXPORT:DwmUnregisterThumbnail=DWMAPI.DwmUnregisterThumbnail,@201")
#pragma comment(linker, "/EXPORT:DwmUpdateThumbnailProperties=DWMAPI.DwmUpdateThumbnailProperties,@202")
#pragma comment(linker, "/EXPORT:DwmpAllocateSecurityDescriptor=DWMAPI.DwmpAllocateSecurityDescriptor,@136")
#pragma comment(linker, "/EXPORT:DwmpDxGetWindowSharedSurface=DWMAPI.DwmpDxGetWindowSharedSurface,@100")
#pragma comment(linker, "/EXPORT:DwmpDxUpdateWindowSharedSurface=DWMAPI.DwmpDxUpdateWindowSharedSurface,@101")
#pragma comment(linker, "/EXPORT:DwmpDxgiIsThreadDesktopComposited=DWMAPI.DwmpDxgiIsThreadDesktopComposited,@128")
#pragma comment(linker, "/EXPORT:DwmpEnableDDASupport=DWMAPI.DwmpEnableDDASupport,@143")
#pragma comment(linker, "/EXPORT:DwmpFreeSecurityDescriptor=DWMAPI.DwmpFreeSecurityDescriptor,@137")
#pragma comment(linker, "/EXPORT:DwmpGetColorizationParameters=DWMAPI.DwmpGetColorizationParameters,@127")
#pragma comment(linker, "/EXPORT:DwmpRenderFlick=DWMAPI.DwmpRenderFlick,@135")
#pragma comment(linker, "/EXPORT:DwmpSetColorizationParameters=DWMAPI.DwmpSetColorizationParameters,@131")
#pragma comment(linker, "/EXPORT:DwmpUpdateProxyWindowForCapture=DWMAPI.DwmpUpdateProxyWindowForCapture,@183")
#pragma comment(linker, "/EXPORT:#103=DWMAPI.#103,@103,NONAME")
#pragma comment(linker, "/EXPORT:#104=DWMAPI.#104,@104,NONAME")
#pragma comment(linker, "/EXPORT:#105=DWMAPI.#105,@105,NONAME")
#pragma comment(linker, "/EXPORT:#106=DWMAPI.#106,@106,NONAME")
#pragma comment(linker, "/EXPORT:#107=DWMAPI.#107,@107,NONAME")
#pragma comment(linker, "/EXPORT:#108=DWMAPI.#108,@108,NONAME")
#pragma comment(linker, "/EXPORT:#109=DWMAPI.#109,@109,NONAME")
#pragma comment(linker, "/EXPORT:#110=DWMAPI.#110,@110,NONAME")
#pragma comment(linker, "/EXPORT:#112=DWMAPI.#112,@112,NONAME")
#pragma comment(linker, "/EXPORT:#113=DWMAPI.#113,@113,NONAME")
#pragma comment(linker, "/EXPORT:#114=DWMAPI.#114,@114,NONAME")
#pragma comment(linker, "/EXPORT:#124=DWMAPI.#124,@124,NONAME")
#pragma comment(linker, "/EXPORT:#132=DWMAPI.#132,@132,NONAME")
#pragma comment(linker, "/EXPORT:#138=DWMAPI.#138,@138,NONAME")
#pragma comment(linker, "/EXPORT:#139=DWMAPI.#139,@139,NONAME")
#pragma comment(linker, "/EXPORT:#140=DWMAPI.#140,@140,NONAME")
#pragma comment(linker, "/EXPORT:#141=DWMAPI.#141,@141,NONAME")
#pragma comment(linker, "/EXPORT:#142=DWMAPI.#142,@142,NONAME")
#pragma comment(linker, "/EXPORT:#144=DWMAPI.#144,@144,NONAME")
#pragma comment(linker, "/EXPORT:#145=DWMAPI.#145,@145,NONAME")
#pragma comment(linker, "/EXPORT:#146=DWMAPI.#146,@146,NONAME")
#pragma comment(linker, "/EXPORT:#147=DWMAPI.#147,@147,NONAME")
#pragma comment(linker, "/EXPORT:#148=DWMAPI.#148,@148,NONAME")
#pragma comment(linker, "/EXPORT:#150=DWMAPI.#150,@150,NONAME")
#pragma comment(linker, "/EXPORT:#151=DWMAPI.#151,@151,NONAME")
#pragma comment(linker, "/EXPORT:#152=DWMAPI.#152,@152,NONAME")
#pragma comment(linker, "/EXPORT:#153=DWMAPI.#153,@153,NONAME")
#pragma comment(linker, "/EXPORT:#154=DWMAPI.#154,@154,NONAME")
#pragma comment(linker, "/EXPORT:#155=DWMAPI.#155,@155,NONAME")
#pragma comment(linker, "/EXPORT:#157=DWMAPI.#157,@157,NONAME")
#pragma comment(linker, "/EXPORT:#158=DWMAPI.#158,@158,NONAME")
#pragma comment(linker, "/EXPORT:#159=DWMAPI.#159,@159,NONAME")
#pragma comment(linker, "/EXPORT:#160=DWMAPI.#160,@160,NONAME")
#pragma comment(linker, "/EXPORT:#161=DWMAPI.#161,@161,NONAME")
#pragma comment(linker, "/EXPORT:#162=DWMAPI.#162,@162,NONAME")
#pragma comment(linker, "/EXPORT:#163=DWMAPI.#163,@163,NONAME")
#pragma comment(linker, "/EXPORT:#164=DWMAPI.#164,@164,NONAME")
#pragma comment(linker, "/EXPORT:#165=DWMAPI.#165,@165,NONAME")
#pragma comment(linker, "/EXPORT:#166=DWMAPI.#166,@166,NONAME")
#pragma comment(linker, "/EXPORT:#167=DWMAPI.#167,@167,NONAME")
#pragma comment(linker, "/EXPORT:#168=DWMAPI.#168,@168,NONAME")
#pragma comment(linker, "/EXPORT:#169=DWMAPI.#169,@169,NONAME")
#pragma comment(linker, "/EXPORT:#170=DWMAPI.#170,@170,NONAME")
#pragma comment(linker, "/EXPORT:#171=DWMAPI.#171,@171,NONAME")
#pragma comment(linker, "/EXPORT:#172=DWMAPI.#172,@172,NONAME")
#pragma comment(linker, "/EXPORT:#173=DWMAPI.#173,@173,NONAME")
#pragma comment(linker, "/EXPORT:#174=DWMAPI.#174,@174,NONAME")
#pragma comment(linker, "/EXPORT:#175=DWMAPI.#175,@175,NONAME")
#pragma comment(linker, "/EXPORT:#176=DWMAPI.#176,@176,NONAME")
#pragma comment(linker, "/EXPORT:#177=DWMAPI.#177,@177,NONAME")
#pragma comment(linker, "/EXPORT:#178=DWMAPI.#178,@178,NONAME")
#pragma comment(linker, "/EXPORT:#179=DWMAPI.#179,@179,NONAME")
#pragma comment(linker, "/EXPORT:#180=DWMAPI.#180,@180,NONAME")
#pragma comment(linker, "/EXPORT:#181=DWMAPI.#181,@181,NONAME")
#pragma comment(linker, "/EXPORT:#182=DWMAPI.#182,@182,NONAME")
#pragma comment(linker, "/EXPORT:#184=DWMAPI.#184,@184,NONAME")
#pragma comment(linker, "/EXPORT:#185=DWMAPI.#185,@185,NONAME")
#pragma comment(linker, "/EXPORT:#186=DWMAPI.#186,@186,NONAME")
#pragma comment(linker, "/EXPORT:#187=DWMAPI.#187,@187,NONAME")

// Only inject when the host process is steam.exe (case-insensitive).
// LoadLibraryA itself guarantees that OpenSteamTool.dll's DllMain
// runs at most once per process, so multiple hijack DLLs can safely
// call this without additional synchronisation.
BOOL OpenSteamToolLoad()
{
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH))
    {
        const char* exeName = strrchr(exePath, '\\');
        exeName = exeName ? exeName + 1 : exePath;
        if (_stricmp(exeName, "steam.exe") != 0)
            return TRUE;   // not Steam — let the proxy load, but don't inject
    }
    return LoadLibraryA("OpenSteamTool.dll") != NULL;
}


BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
        {
            DisableThreadLibraryCalls(hModule);
            if ( !OpenSteamToolLoad() )
                return FALSE;
            break;
        }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

>>>>>>> 2a08b0bfdc96c9e3f75df20f3f37bc8d8c09ee3a
