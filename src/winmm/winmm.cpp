// winmm.dll hijack - dynamic System32 wrapper and OpenSteamTool bootstrap.
//
// winmm exports a large, mostly ordinal-stable API surface.  The proxy keeps
// that surface intact by forwarding every export through its System32 ordinal
// and only performs the OpenSteamTool bootstrap when the host is steam.exe.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <mutex>

#include "../Common/ProxyBootstrap.hpp"

namespace {

static_assert(sizeof(void*) == 8, "winmm proxy requires the 64-bit Steam process");

constexpr WORD kFirstWinmmOrdinal = 2;
constexpr WORD kLastWinmmOrdinal = 182;

using WinmmProc = INT_PTR(WINAPI*)(
    void*, void*, void*, void*, void*, void*, void*, void*);

std::once_flag g_winmmInitOnce;
HMODULE g_realWinmm = nullptr;
std::array<FARPROC, kLastWinmmOrdinal + 1> g_exports{};

void LoadRealWinmm() noexcept {
    std::call_once(g_winmmInitOnce, []() noexcept {
        g_realWinmm = OST::Proxy::SystemDllLoader::Load(L"winmm.dll");
        if (!g_realWinmm) {
            return;
        }

        for (WORD ordinal = kFirstWinmmOrdinal; ordinal <= kLastWinmmOrdinal; ++ordinal) {
            g_exports[ordinal] = ::GetProcAddress(g_realWinmm, MAKEINTRESOURCEA(ordinal));
        }
    });
}

INT_PTR ForwardWinmm(
    WORD ordinal,
    void* a1,
    void* a2,
    void* a3,
    void* a4,
    void* a5,
    void* a6,
    void* a7,
    void* a8) noexcept {
    // Calling call_once on every entry is cheap after initialization and also
    // provides the acquire/release synchronization for the dispatch table.
    LoadRealWinmm();

    if (ordinal > kLastWinmmOrdinal) {
        return 0;
    }

    const auto proc = reinterpret_cast<WinmmProc>(g_exports[ordinal]);
    return proc ? proc(a1, a2, a3, a4, a5, a6, a7, a8) : 0;
}

} // namespace

#define FORWARD_WINMM(name, ordinal)                                                     \
    extern "C" INT_PTR WINAPI name(                                                       \
        void* a1, void* a2, void* a3, void* a4,                                          \
        void* a5, void* a6, void* a7, void* a8) {                                         \
        return ForwardWinmm(ordinal, a1, a2, a3, a4, a5, a6, a7, a8);                     \
    }

FORWARD_WINMM(WinmmOrdinal2, 2)
FORWARD_WINMM(CloseDriver, 4)
FORWARD_WINMM(DefDriverProc, 5)
FORWARD_WINMM(DriverCallback, 6)
FORWARD_WINMM(DrvGetModuleHandle, 7)
FORWARD_WINMM(GetDriverModuleHandle, 8)
FORWARD_WINMM(OpenDriver, 9)
FORWARD_WINMM(PlaySound, 10)
FORWARD_WINMM(PlaySoundA, 11)
FORWARD_WINMM(PlaySoundW, 12)
FORWARD_WINMM(SendDriverMessage, 13)
FORWARD_WINMM(WOWAppExit, 14)
FORWARD_WINMM(auxGetDevCapsA, 15)
FORWARD_WINMM(auxGetDevCapsW, 16)
FORWARD_WINMM(auxGetNumDevs, 17)
FORWARD_WINMM(auxGetVolume, 18)
FORWARD_WINMM(auxOutMessage, 19)
FORWARD_WINMM(auxSetVolume, 20)
FORWARD_WINMM(joyConfigChanged, 21)
FORWARD_WINMM(joyGetDevCapsA, 22)
FORWARD_WINMM(joyGetDevCapsW, 23)
FORWARD_WINMM(joyGetNumDevs, 24)
FORWARD_WINMM(joyGetPos, 25)
FORWARD_WINMM(joyGetPosEx, 26)
FORWARD_WINMM(joyGetThreshold, 27)
FORWARD_WINMM(joyReleaseCapture, 28)
FORWARD_WINMM(joySetCapture, 29)
FORWARD_WINMM(joySetThreshold, 30)
FORWARD_WINMM(mciDriverNotify, 31)
FORWARD_WINMM(mciDriverYield, 32)
FORWARD_WINMM(mciExecute, 3)
FORWARD_WINMM(mciFreeCommandResource, 33)
FORWARD_WINMM(mciGetCreatorTask, 34)
FORWARD_WINMM(mciGetDeviceIDA, 35)
FORWARD_WINMM(mciGetDeviceIDFromElementIDA, 36)
FORWARD_WINMM(mciGetDeviceIDFromElementIDW, 37)
FORWARD_WINMM(mciGetDeviceIDW, 38)
FORWARD_WINMM(mciGetDriverData, 39)
FORWARD_WINMM(mciGetErrorStringA, 40)
FORWARD_WINMM(mciGetErrorStringW, 41)
FORWARD_WINMM(mciGetYieldProc, 42)
FORWARD_WINMM(mciLoadCommandResource, 43)
FORWARD_WINMM(mciSendCommandA, 44)
FORWARD_WINMM(mciSendCommandW, 45)
FORWARD_WINMM(mciSendStringA, 46)
FORWARD_WINMM(mciSendStringW, 47)
FORWARD_WINMM(mciSetDriverData, 48)
FORWARD_WINMM(mciSetYieldProc, 49)
FORWARD_WINMM(midiConnect, 50)
FORWARD_WINMM(midiDisconnect, 51)
FORWARD_WINMM(midiInAddBuffer, 52)
FORWARD_WINMM(midiInClose, 53)
FORWARD_WINMM(midiInGetDevCapsA, 54)
FORWARD_WINMM(midiInGetDevCapsW, 55)
FORWARD_WINMM(midiInGetErrorTextA, 56)
FORWARD_WINMM(midiInGetErrorTextW, 57)
FORWARD_WINMM(midiInGetID, 58)
FORWARD_WINMM(midiInGetNumDevs, 59)
FORWARD_WINMM(midiInMessage, 60)
FORWARD_WINMM(midiInOpen, 61)
FORWARD_WINMM(midiInPrepareHeader, 62)
FORWARD_WINMM(midiInReset, 63)
FORWARD_WINMM(midiInStart, 64)
FORWARD_WINMM(midiInStop, 65)
FORWARD_WINMM(midiInUnprepareHeader, 66)
FORWARD_WINMM(midiOutCacheDrumPatches, 67)
FORWARD_WINMM(midiOutCachePatches, 68)
FORWARD_WINMM(midiOutClose, 69)
FORWARD_WINMM(midiOutGetDevCapsA, 70)
FORWARD_WINMM(midiOutGetDevCapsW, 71)
FORWARD_WINMM(midiOutGetErrorTextA, 72)
FORWARD_WINMM(midiOutGetErrorTextW, 73)
FORWARD_WINMM(midiOutGetID, 74)
FORWARD_WINMM(midiOutGetNumDevs, 75)
FORWARD_WINMM(midiOutGetVolume, 76)
FORWARD_WINMM(midiOutLongMsg, 77)
FORWARD_WINMM(midiOutMessage, 78)
FORWARD_WINMM(midiOutOpen, 79)
FORWARD_WINMM(midiOutPrepareHeader, 80)
FORWARD_WINMM(midiOutReset, 81)
FORWARD_WINMM(midiOutSetVolume, 82)
FORWARD_WINMM(midiOutShortMsg, 83)
FORWARD_WINMM(midiOutUnprepareHeader, 84)
FORWARD_WINMM(midiStreamClose, 85)
FORWARD_WINMM(midiStreamOpen, 86)
FORWARD_WINMM(midiStreamOut, 87)
FORWARD_WINMM(midiStreamPause, 88)
FORWARD_WINMM(midiStreamPosition, 89)
FORWARD_WINMM(midiStreamProperty, 90)
FORWARD_WINMM(midiStreamRestart, 91)
FORWARD_WINMM(midiStreamStop, 92)
FORWARD_WINMM(mixerClose, 93)
FORWARD_WINMM(mixerGetControlDetailsA, 94)
FORWARD_WINMM(mixerGetControlDetailsW, 95)
FORWARD_WINMM(mixerGetDevCapsA, 96)
FORWARD_WINMM(mixerGetDevCapsW, 97)
FORWARD_WINMM(mixerGetID, 98)
FORWARD_WINMM(mixerGetLineControlsA, 99)
FORWARD_WINMM(mixerGetLineControlsW, 100)
FORWARD_WINMM(mixerGetLineInfoA, 101)
FORWARD_WINMM(mixerGetLineInfoW, 102)
FORWARD_WINMM(mixerGetNumDevs, 103)
FORWARD_WINMM(mixerMessage, 104)
FORWARD_WINMM(mixerOpen, 105)
FORWARD_WINMM(mixerSetControlDetails, 106)
FORWARD_WINMM(mmDrvInstall, 107)
FORWARD_WINMM(mmGetCurrentTask, 108)
FORWARD_WINMM(mmTaskBlock, 109)
FORWARD_WINMM(mmTaskCreate, 110)
FORWARD_WINMM(mmTaskSignal, 111)
FORWARD_WINMM(mmTaskYield, 112)
FORWARD_WINMM(mmioAdvance, 113)
FORWARD_WINMM(mmioAscend, 114)
FORWARD_WINMM(mmioClose, 115)
FORWARD_WINMM(mmioCreateChunk, 116)
FORWARD_WINMM(mmioDescend, 117)
FORWARD_WINMM(mmioFlush, 118)
FORWARD_WINMM(mmioGetInfo, 119)
FORWARD_WINMM(mmioInstallIOProcA, 120)
FORWARD_WINMM(mmioInstallIOProcW, 121)
FORWARD_WINMM(mmioOpenA, 122)
FORWARD_WINMM(mmioOpenW, 123)
FORWARD_WINMM(mmioRead, 124)
FORWARD_WINMM(mmioRenameA, 125)
FORWARD_WINMM(mmioRenameW, 126)
FORWARD_WINMM(mmioSeek, 127)
FORWARD_WINMM(mmioSendMessage, 128)
FORWARD_WINMM(mmioSetBuffer, 129)
FORWARD_WINMM(mmioSetInfo, 130)
FORWARD_WINMM(mmioStringToFOURCCA, 131)
FORWARD_WINMM(mmioStringToFOURCCW, 132)
FORWARD_WINMM(mmioWrite, 133)
FORWARD_WINMM(mmsystemGetVersion, 134)
FORWARD_WINMM(sndPlaySoundA, 135)
FORWARD_WINMM(sndPlaySoundW, 136)
FORWARD_WINMM(timeBeginPeriod, 137)
FORWARD_WINMM(timeEndPeriod, 138)
FORWARD_WINMM(timeGetDevCaps, 139)
FORWARD_WINMM(timeGetSystemTime, 140)
FORWARD_WINMM(timeGetTime, 141)
FORWARD_WINMM(timeKillEvent, 142)
FORWARD_WINMM(timeSetEvent, 143)
FORWARD_WINMM(waveInAddBuffer, 144)
FORWARD_WINMM(waveInClose, 145)
FORWARD_WINMM(waveInGetDevCapsA, 146)
FORWARD_WINMM(waveInGetDevCapsW, 147)
FORWARD_WINMM(waveInGetErrorTextA, 148)
FORWARD_WINMM(waveInGetErrorTextW, 149)
FORWARD_WINMM(waveInGetID, 150)
FORWARD_WINMM(waveInGetNumDevs, 151)
FORWARD_WINMM(waveInGetPosition, 152)
FORWARD_WINMM(waveInMessage, 153)
FORWARD_WINMM(waveInOpen, 154)
FORWARD_WINMM(waveInPrepareHeader, 155)
FORWARD_WINMM(waveInReset, 156)
FORWARD_WINMM(waveInStart, 157)
FORWARD_WINMM(waveInStop, 158)
FORWARD_WINMM(waveInUnprepareHeader, 159)
FORWARD_WINMM(waveOutBreakLoop, 160)
FORWARD_WINMM(waveOutClose, 161)
FORWARD_WINMM(waveOutGetDevCapsA, 162)
FORWARD_WINMM(waveOutGetDevCapsW, 163)
FORWARD_WINMM(waveOutGetErrorTextA, 164)
FORWARD_WINMM(waveOutGetErrorTextW, 165)
FORWARD_WINMM(waveOutGetID, 166)
FORWARD_WINMM(waveOutGetNumDevs, 167)
FORWARD_WINMM(waveOutGetPitch, 168)
FORWARD_WINMM(waveOutGetPlaybackRate, 169)
FORWARD_WINMM(waveOutGetPosition, 170)
FORWARD_WINMM(waveOutGetVolume, 171)
FORWARD_WINMM(waveOutMessage, 172)
FORWARD_WINMM(waveOutOpen, 173)
FORWARD_WINMM(waveOutPause, 174)
FORWARD_WINMM(waveOutPrepareHeader, 175)
FORWARD_WINMM(waveOutReset, 176)
FORWARD_WINMM(waveOutRestart, 177)
FORWARD_WINMM(waveOutSetPitch, 178)
FORWARD_WINMM(waveOutSetPlaybackRate, 179)
FORWARD_WINMM(waveOutSetVolume, 180)
FORWARD_WINMM(waveOutUnprepareHeader, 181)
FORWARD_WINMM(waveOutWrite, 182)

#undef FORWARD_WINMM

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, PVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        return OST::Proxy::Bootstrap::OnAttach(hModule);
    }
    return TRUE;
}
