#ifndef DLLMAIN_H
#define DLLMAIN_H

#include "OSTPlatform/include/DynamicLibrary.h"

#include <string>
#include <fstream>
#include <filesystem>
#include <array>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <regex>
#include <memory>
#include <atomic>
#include <format>

#include "Steam/Types.h"
#include "Steam/Enums.h"
#include "Steam/Structs.h"
#include "Steam/Callback.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "Utils/Config/Config.h"

inline std::atomic<OSTPlatform::DynamicLibrary::ModuleHandle> client_hModule{nullptr};
inline std::atomic<OSTPlatform::DynamicLibrary::ModuleHandle> ui_hModule{nullptr};

inline std::atomic<bool> g_HooksInstalled{false};
inline std::atomic<bool> g_IsDiversionActive{false};

inline constexpr size_t kRuntimePathCapacity = 1024;

inline char SteamInstallPath[kRuntimePathCapacity] = {};
inline char SteamclientPath[kRuntimePathCapacity] = {};
inline char SteamUIPath[kRuntimePathCapacity] = {};
inline char DiversionPath[kRuntimePathCapacity] = {};
inline char LuaDir[kRuntimePathCapacity] = {};
inline char ConfigPath[kRuntimePathCapacity] = {};

// The fake AppId used by -onlinefix (SpaceWar).
constexpr AppId_t kOnlineFixAppId = 480;

// 家庭共享 + lua 的游戏在上报时改成这个 AppId，避免服务器按共享许可记账。
// 取一个不可能存在的 ID：真实存在的 AppId 会被服务器照常播报出去，
// 只有查不到的这个才既不占名额、也不出现在好友侧。
constexpr AppId_t kFamilySharedMaskAppId = 0x7FFFFFFF;

#endif // DLLMAIN_H
