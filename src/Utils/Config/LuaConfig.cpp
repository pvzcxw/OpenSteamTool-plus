#include "dllmain.h"
#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/SteamMetadata/StatsClient.h"
#include "Utils/Tickets/AppTicket.h"

#include <lua.hpp>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

extern "C" {
    struct lua_State;
}

namespace LuaConfig{
    static lua_State* g_lua_state = nullptr;
    static bool g_hasManifestCodeFunc = false;
    static bool g_hasManifestCodeFuncEx = false;
    static std::recursive_mutex g_luaMutex;
    static std::shared_mutex g_depotDataMutex;
    std::unordered_map<AppId_t, std::string>DepotKeySet{};
    std::unordered_map<AppId_t, uint64_t>AccessTokenSet{};
    std::unordered_set<AppId_t> PinnedApps{};
    std::unordered_map<uint64_t, ManifestOverride> ManifestOverrides{};
    std::unordered_map<AppId_t, uint64_t> StatSteamIdSet{};
    std::unordered_set<AppId_t> OwnedAppIdSet{};
    static std::unordered_set<AppId_t> ForcedDenuvoSet{};
    static std::unordered_set<AppId_t> ForcedNonDenuvoSet{};

    // Per-file tracking: which depots each .lua file contributed.
    static std::string g_currentFile;
    static std::unordered_map<std::string, std::unordered_set<AppId_t>> g_fileDepots;
    // Keys are tracked per source file so removing/reloading one script cannot
    // leave its key active merely because another script still lists the depot.
    static std::unordered_map<std::string, std::unordered_map<AppId_t, std::string>> g_fileDepotKeys;
    static std::unordered_set<AppId_t> g_untrackedDepotIds;
    static std::unordered_map<AppId_t, std::string> g_untrackedDepotKeys;
    static std::unordered_map<std::string, std::unordered_map<uint64_t, ManifestOverride>> g_fileManifestOverrides;
    static std::unordered_map<std::string, uint64_t> g_fileParseSequence;
    static uint64_t g_nextFileParseSequence = 0;
    // Reference count: how many files provide each depot.
    static std::unordered_map<AppId_t, uint32_t> g_depotRefCount;
    // mtime (unix epoch seconds) of each parsed .lua file, captured at ParseFile entry.
    static std::unordered_map<std::string, uint32_t> g_fileMtime;
    // Per-appId purchase time: max(mtime) across every file that currently contributes it.
    // Simple variant: never lowered on UnloadFile unless the refcount drops to zero.
    static std::unordered_map<AppId_t, uint32_t> g_purchaseTime;
    // Depot IDs removed by UnloadFile / added by ParseFile, consumed by NotifyLicenseChanged.
    static std::vector<AppId_t> g_pendingRemovals;
    static std::vector<AppId_t> g_pendingAdditions;
    constexpr uint64_t kDefaultStatSteamId = 76561198028121353ULL;

    // Caller holds g_depotDataMutex exclusively.
    static void RebuildDepotKeyLocked(AppId_t depotId) {
        const std::string* bestKey = nullptr;
        uint64_t bestSequence = 0;

        if (const auto it = g_untrackedDepotKeys.find(depotId);
            it != g_untrackedDepotKeys.end()) {
            bestKey = &it->second;
        }

        for (const auto& [file, keys] : g_fileDepotKeys) {
            const auto keyIt = keys.find(depotId);
            const auto sequenceIt = g_fileParseSequence.find(file);
            if (keyIt == keys.end() || sequenceIt == g_fileParseSequence.end()) continue;
            if (!bestKey || sequenceIt->second > bestSequence) {
                bestKey = &keyIt->second;
                bestSequence = sequenceIt->second;
            }
        }

        const bool depotStillListed = g_untrackedDepotIds.contains(depotId) ||
                                      g_depotRefCount.contains(depotId);
        if (bestKey) {
            DepotKeySet[depotId] = *bestKey;
        } else if (depotStillListed) {
            DepotKeySet[depotId].clear();
        } else {
            DepotKeySet.erase(depotId);
        }
    }

    static bool IsValidDepotKey(std::string_view key) {
        if (key.size() != 64) return false;
        for (const unsigned char ch : key) {
            const bool hex = (ch >= '0' && ch <= '9') ||
                             (ch >= 'a' && ch <= 'f') ||
                             (ch >= 'A' && ch <= 'F');
            if (!hex) return false;
        }
        return true;
    }

    // Case-insensitive function registry: lowercase name → C function
    static std::unordered_map<std::string, lua_CFunction> g_func_registry;

    static bool ParseUInt64Decimal(std::string_view text, uint64_t* out) {
        if (!out) return false;
        const auto parsed = OSTPlatform::Numbers::ParseUInt64(text);
        if (!parsed) return false;
        *out = *parsed;
        return true;
    }

    static void PushUInt64(lua_State* L, uint64_t value) {
        if (value <= static_cast<uint64_t>(std::numeric_limits<lua_Integer>::max())) {
            lua_pushinteger(L, static_cast<lua_Integer>(value));
            return;
        }
        const std::string decimal = std::to_string(value);
        lua_pushlstring(L, decimal.data(), decimal.size());
    }

    static uint8 ParseHexByte(std::string_view text) {
        return OSTPlatform::Numbers::ParseHexUInt8(text).value_or(0);
    }

    static void SetActiveManifestOverride(uint64_t depotId, const ManifestOverride& override) {
        std::lock_guard lock(g_luaMutex);
        ManifestOverrides[depotId] = override;
    }

    static void ClearActiveManifestOverride(uint64_t depotId) {
        std::lock_guard lock(g_luaMutex);
        ManifestOverrides.erase(depotId);
    }

    static void RebuildManifestOverride(uint64_t depotId) {
        std::lock_guard lock(g_luaMutex);
        const ManifestOverride* best = nullptr;
        uint64_t bestSeq = 0;

        for (const auto& [file, overrides] : g_fileManifestOverrides) {
            auto overrideIt = overrides.find(depotId);
            if (overrideIt == overrides.end()) continue;

            auto seqIt = g_fileParseSequence.find(file);
            if (seqIt == g_fileParseSequence.end()) continue;

            if (!best || seqIt->second > bestSeq) {
                best = &overrideIt->second;
                bestSeq = seqIt->second;
            }
        }

        if (best) {
            SetActiveManifestOverride(depotId, *best);
        } else {
            ClearActiveManifestOverride(depotId);
        }
    }

    // ── Lua HTTP helpers ──────────────────────────────────────────
    //   http_get(url [, headers]) → body, status_code
    //     headers: optional table, e.g. {["Accept"]="application/json"}
    //
    //   http_post(url, post_body [, headers]) → body, status_code
    //     post_body: string payload
    // ────────────────────────────────────────────────────────────────

    // Serialise a Lua {key=val, ...} table at stack index idx into a
    // wstring of "Key: Val\r\n" lines. Returns L"" if no table.
    static std::wstring LuaHeadersToWstr(lua_State* L, int idx) {
        std::wstring headers;
        if (idx < 1 || !lua_istable(L, idx)) return headers;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            std::string key(lua_tostring(L, -2));
            std::string val(lua_tostring(L, -1));
            headers += std::wstring(key.begin(), key.end())
                    + L": " + std::wstring(val.begin(), val.end()) + L"\r\n";
            lua_pop(L, 1);
        }
        return headers;
    }

    static int lua_http_get(lua_State* L) {
        // http_get(url [, headers]) → body, status_code
        auto hdrs = LuaHeadersToWstr(L, lua_gettop(L) >= 2 ? 2 : -1);
        auto r = OSTPlatform::Http::Execute(L"GET", luaL_checkstring(L, 1),
                                            nullptr, 0, hdrs.empty() ? nullptr : hdrs.c_str());
        if (r.ok) {
            lua_pushstring(L, r.body.c_str());
            lua_pushinteger(L, r.status);
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "HTTP request failed");
        }
        return 2;
    }

    static int lua_http_post(lua_State* L) {
        // http_post(url, post_body [, headers]) → body, status_code
        size_t bodyLen = 0;
        const char* body = luaL_checklstring(L, 2, &bodyLen);
        auto hdrs = LuaHeadersToWstr(L, lua_gettop(L) >= 3 ? 3 : -1);
        auto r = OSTPlatform::Http::Execute(L"POST", luaL_checkstring(L, 1),
                                            body, static_cast<uint32_t>(bodyLen),
                                            hdrs.empty() ? nullptr : hdrs.c_str());
        if (r.ok) {
            lua_pushstring(L, r.body.c_str());
            lua_pushinteger(L, r.status);
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "HTTP request failed");
        }
        return 2;
    }

    // ── Case-insensitive global function lookup ─────────────────
    // __index metamethod on _G: when a global name isn't found,
    // lower-case the name and look it up in g_func_registry.
    static int case_insensitive_global_index(lua_State* L) {
        const char* name = lua_tostring(L, 2);
        if (!name) {
            lua_pushnil(L);
            return 1;
        }
        std::string lower;
        for (const char* p = name; *p; ++p) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        }
        auto it = g_func_registry.find(lower);
        if (it != g_func_registry.end()) {
            lua_pushcfunction(L, it->second);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    // Register a C function both in _G (lowercase name) and in the
    // case-insensitive lookup table so any case variant resolves.
    static void register_func(lua_State* L, const char* lowercase_name, lua_CFunction fn) {
        g_func_registry[lowercase_name] = fn;
        lua_pushcfunction(L, fn);
        lua_setglobal(L, lowercase_name);
    }

    // ── Lua: addappid / addtoken / pinApp / setManifestid ────────
    static int lua_addappid(lua_State* L) {
        // addappid(integer, integer, string)
        int argc = lua_gettop(L);
        // Validate argument count and required argument types.
        if (argc == 0) {
            return luaL_error(L, "");
        }
        if (!lua_isinteger(L, 1)) {
            return luaL_error(L, "");
        }

        // Read the first argument as app/depot id.
        lua_Integer value = lua_tointeger(L, 1);
        // Ensure the value fits into uint32_t range.
        if (value < 0 || value > UINT32_MAX)
            return luaL_error(L, "");
        AppId_t DepotId = (uint32_t)value;
        // Read the optional third argument as a key.
        std::string Key = "";
        if (argc > 2) {
            if (lua_type(L, 3) != LUA_TSTRING)
                return luaL_error(L, "addappid: key must be a string");
            size_t keyLength = 0;
            const char* key = lua_tolstring(L, 3, &keyLength);
            if (keyLength != 0) {
                const std::string_view keyView(key, keyLength);
                if (!IsValidDepotKey(keyView))
                    return luaL_error(L, "addappid: key must be exactly 64 hexadecimal characters");
                Key.assign(keyView);
            }
        }

        std::unique_lock lock(g_depotDataMutex);
        bool keyChanged = false;
        if (!g_currentFile.empty()) {
            if (g_fileDepots[g_currentFile].insert(DepotId).second) {
                if (++g_depotRefCount[DepotId] == 1)
                    g_pendingAdditions.push_back(DepotId);
                
                // Update the appId's purchase time with the current file's mtime,
                // keeping the maximum across every contributing file.
                auto mtIt = g_fileMtime.find(g_currentFile);
                if (mtIt != g_fileMtime.end()) {
                    uint32_t mt = mtIt->second;
                    auto& slot = g_purchaseTime[DepotId];
                    if (mt > slot) slot = mt;
                }
            }

            if (!Key.empty()) {
                g_fileDepotKeys[g_currentFile][DepotId] = Key;
                keyChanged = true;
            }
        } else {
            g_untrackedDepotIds.insert(DepotId);
            if (!Key.empty()) {
                g_untrackedDepotKeys[DepotId] = Key;
                keyChanged = true;
            }
        }

        // A missing key remains a valid depot declaration, but it must not
        // shadow a non-empty key contributed by another Lua file.
        if (keyChanged || !DepotKeySet.contains(DepotId))
            RebuildDepotKeyLocked(DepotId);

        return 0;
    }

    static int lua_addtoken(lua_State* L) {
        // addtoken(integer, string(uint64_t))
        int argc = lua_gettop(L);
        // Validate argument count and required argument types.
        if (argc == 0) {
            return luaL_error(L, "");
        }
        if (!lua_isinteger(L, 1)) {
            return luaL_error(L, "");
        }

        // Read the first argument as app/depot id.
        lua_Integer value = lua_tointeger(L, 1);
        // Ensure the value fits into uint32_t range.
        if (value < 0 || value > UINT32_MAX)
            return luaL_error(L, "");
        AppId_t AppId = (uint32_t)value;
        // Read the second argument as a token.
        if (argc > 1) {
            if (!lua_isstring(L, 2))
                return luaL_error(L, "");
            size_t tokenLength = 0;
            const char* token = lua_tolstring(L, 2, &tokenLength);
            uint64_t parsedToken = 0;
            if (!ParseUInt64Decimal(std::string_view(token, tokenLength), &parsedToken)) {
                return luaL_error(L, "");
            }
            AccessTokenSet[AppId] = parsedToken;
        }

        return 0;
    }

    static int lua_pinApp(lua_State* L) {
        // pinApp(integer)
        int argc = lua_gettop(L);
        // Validate argument count and required argument types.
        if (argc == 0) {
            return luaL_error(L, "");
        }
        if (!lua_isinteger(L, 1)) {
            return luaL_error(L, "");
        }

        // Read the first argument as appid.
        lua_Integer value = lua_tointeger(L, 1);
        // Ensure the value fits into uint32_t range.
        if (value < 0 || value > UINT32_MAX)
            return luaL_error(L, "");
        AppId_t AppId = (uint32_t)value;

        PinnedApps.insert(AppId);

        return 0;
    }

    static int lua_setManifestid(lua_State* L)
    {
        // setManifestid(depotId, gid_string [, size])
        // size is always forced to 0 to prevent incorrect size from breaking Steam.
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setManifestid: need depotId, gid");

        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setManifestid: depotId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setManifestid: gid must be decimal string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setManifestid: depotId out of range");

        uint64_t depotId = (uint64_t)(uint32_t)val;
        size_t gidLength = 0;
        const char* gidStr = lua_tolstring(L, 2, &gidLength);

        uint64_t gid = 0;
        if (!ParseUInt64Decimal(std::string_view(gidStr, gidLength), &gid) || gid == 0)
            return luaL_error(L, "setManifestid: gid must be all digits");

        ManifestOverride override{ gid, 0 };
        if (!g_currentFile.empty()) {
            g_fileManifestOverrides[g_currentFile][depotId] = override;
            RebuildManifestOverride(depotId);
        } else {
            SetActiveManifestOverride(depotId, override);
        }
        return 0;
    }

    // ── Lua: setAppTicket / setETicket ──────────────────────────
    static int lua_setAppticket(lua_State* L) {
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setAppTicket: need appId and hex string");
        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setAppTicket: appId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setAppTicket: ticket must be hex string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setAppTicket: appId out of range");
        AppId_t appId = static_cast<uint32_t>(val);

        size_t hexLen;
        const char* hex = lua_tolstring(L, 2, &hexLen);

        std::vector<uint8_t> binary;
        binary.reserve((hexLen + 1) / 2);
        for (size_t i = 0; i < hexLen; i += 2) {
            char byteStr[3] = {
                hex[i],
                i + 1 < hexLen ? hex[i + 1] : '0',
                '\0'
            };
            binary.push_back(ParseHexByte(byteStr));
        }

        if (!AppTicket::WriteAppOwnershipTicket(appId, binary))
            return luaL_error(L, "setAppTicket: failed to write credential store");

        return 0;
    }

    static int lua_setEticket(lua_State* L) {
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setETicket: need appId and hex string");
        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setETicket: appId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setETicket: ticket must be hex string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setETicket: appId out of range");
        AppId_t appId = static_cast<uint32_t>(val);

        size_t hexLen;
        const char* hex = lua_tolstring(L, 2, &hexLen);

        std::vector<uint8_t> binary;
        binary.reserve((hexLen + 1) / 2);
        for (size_t i = 0; i < hexLen; i += 2) {
            char byteStr[3] = {
                hex[i],
                i + 1 < hexLen ? hex[i + 1] : '0',
                '\0'
            };
            binary.push_back(ParseHexByte(byteStr));
        }

        if (!AppTicket::WriteEncryptedTicket(appId, binary))
            return luaL_error(L, "setETicket: failed to write credential store");

        return 0;
    }

    
    // ── Lua: setStat ────────────────────────────────────────────
    static int lua_setStat(lua_State* L) {
        // setStat(appid, "steamid")
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setStat: need appId and steamId string");
        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setStat: appId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setStat: steamId must be string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setStat: appId out of range");
        AppId_t appId = static_cast<uint32_t>(val);

        const char* sidStr = lua_tostring(L, 2);
        size_t sidLength = 0;
        sidStr = lua_tolstring(L, 2, &sidLength);
        uint64_t steamId = 0;
        if (!ParseUInt64Decimal(std::string_view(sidStr, sidLength), &steamId))
            return luaL_error(L, "setStat: steamId must be all digits");

        StatSteamIdSet[appId] = steamId;
        return 0;
    }

    static int lua_forcedenuvo(lua_State* L) {
        if (lua_gettop(L) < 1 || !lua_isinteger(L, 1))
            return luaL_error(L, "forcedenuvo requires (appid: integer[, force: boolean])");
        const lua_Integer value = lua_tointeger(L, 1);
        if (value <= 0 || value > static_cast<lua_Integer>(UINT32_MAX))
            return luaL_error(L, "forcedenuvo: appid out of range");
        if (lua_gettop(L) >= 2 && !lua_isboolean(L, 2))
            return luaL_error(L, "forcedenuvo: force must be a boolean");

        const AppId_t appId = static_cast<AppId_t>(value);
        const bool force = lua_gettop(L) < 2 || lua_toboolean(L, 2) != 0;
        if (force) {
            ForcedNonDenuvoSet.erase(appId);
            ForcedDenuvoSet.insert(appId);
        } else {
            ForcedDenuvoSet.erase(appId);
            ForcedNonDenuvoSet.insert(appId);
        }
        return 0;
    }

    // ── init / cleanup ───────────────────────────────────────────
    static bool Initialize() {
        std::lock_guard lock(g_luaMutex);
        if (g_lua_state)
            return true;
        g_lua_state = luaL_newstate();
        if (!g_lua_state)
            return false;
        // Load standard Lua libraries.
        luaL_openlibs(g_lua_state);

        // Set up case-insensitive global lookup via __index on _G's metatable.
        lua_getglobal(g_lua_state, "_G");
        if (!lua_getmetatable(g_lua_state, -1)) {
            lua_newtable(g_lua_state);
        }
        lua_pushcfunction(g_lua_state, case_insensitive_global_index);
        lua_setfield(g_lua_state, -2, "__index");
        lua_setmetatable(g_lua_state, -2);
        lua_pop(g_lua_state, 1);  // pop _G

        // Register custom helper functions for scripts.
        // All functions use register_func() so any case variant works
        // (e.g. setAppTICKET, addAppId, SETManifestid, etc.).
        register_func(g_lua_state, "addappid", lua_addappid);
        register_func(g_lua_state, "addtoken", lua_addtoken);
        // we don't need it?
        // register_func(g_lua_state, "pinapp", lua_pinApp);
        register_func(g_lua_state, "setmanifestid", lua_setManifestid);
        register_func(g_lua_state, "http_get", lua_http_get);
        register_func(g_lua_state, "http_post", lua_http_post);
        register_func(g_lua_state, "setappticket", lua_setAppticket);
        register_func(g_lua_state, "seteticket", lua_setEticket);
        register_func(g_lua_state, "setstat", lua_setStat);
        register_func(g_lua_state, "forcedenuvo", lua_forcedenuvo);
        return true;
    }

    static void Cleanup() {
        std::lock_guard lock(g_luaMutex);
        if (g_lua_state) {
            lua_close(g_lua_state);
            g_lua_state = nullptr;
        }
        g_hasManifestCodeFunc = false;
        g_hasManifestCodeFuncEx = false;
    }

    // ── public query API ─────────────────────────────────────────
    bool HasDepot(AppId_t DepotId,bool excludeOwned) {
        std::shared_lock lock(g_depotDataMutex);
        return DepotKeySet.contains(DepotId) &&
               (!excludeOwned || !OwnedAppIdSet.contains(DepotId));
    }

    bool IsOwned(AppId_t AppId) {
        std::shared_lock lock(g_depotDataMutex);
        return OwnedAppIdSet.contains(AppId);
    }

    bool IsForcedDenuvo(AppId_t appId) {
        std::lock_guard lock(g_luaMutex);
        return ForcedDenuvoSet.count(appId) > 0;
    }

    bool IsForcedNonDenuvo(AppId_t appId) {
        std::lock_guard lock(g_luaMutex);
        return ForcedNonDenuvoSet.count(appId) > 0;
    }

    void MarkOwned(AppId_t AppId) {
        std::unique_lock lock(g_depotDataMutex);
        if(!OwnedAppIdSet.count(AppId)) {
            LOG_PACKAGE_INFO("Marking app {} as owned", AppId);
            OwnedAppIdSet.insert(AppId);
        }
    }

    std::vector<AppId_t> GetAllDepotIds() {
        std::shared_lock lock(g_depotDataMutex);
        std::vector<AppId_t> DepotIds;
        DepotIds.reserve(DepotKeySet.size());
        for (const auto& pair : DepotKeySet) {
            DepotIds.push_back(pair.first);
        }
        return DepotIds;
    }

    std::vector<uint8> GetDecryptionKey(AppId_t DepotId) {
        std::vector<uint8> keyBytes;
        std::shared_lock lock(g_depotDataMutex);
        const auto it = DepotKeySet.find(DepotId);
        if (it == DepotKeySet.end() || !IsValidDepotKey(it->second)) return keyBytes;

        keyBytes.reserve(it->second.size() / 2);
        for (size_t i = 0; i < it->second.size(); i += 2)
            keyBytes.push_back(ParseHexByte(std::string_view(it->second).substr(i, 2)));
        return keyBytes;
    }

    uint64_t GetAccessToken(AppId_t AppId) {
        std::lock_guard lock(g_luaMutex);
        if (AccessTokenSet.count(AppId)) {
            return AccessTokenSet[AppId];
        }
        return 0;
    }

    bool pinApp(AppId_t AppId) {
        return PinnedApps.count(AppId);
    }

    uint64_t GetStatSteamId(AppId_t AppId) {
        {
            std::lock_guard lock(g_luaMutex);
            const auto it = StatSteamIdSet.find(AppId);
            if (it != StatSteamIdSet.end()) return it->second;
        }
        uint64_t apiSteamId = 0;
        if (StatsClient::FetchStatSteamId(AppId, &apiSteamId))
            return apiSteamId;
        return kDefaultStatSteamId;
    }

    uint32_t GetPurchaseTime(AppId_t AppId) {
        std::shared_lock lock(g_depotDataMutex);
        auto it = g_purchaseTime.find(AppId);
        return it != g_purchaseTime.end() ? it->second : 0;
    }

    std::unordered_map<uint64_t, ManifestOverride> GetManifestOverrides() {
        std::lock_guard lock(g_luaMutex);
        return ManifestOverrides;
    }

    bool HasManifestCodeFunc() {
        std::lock_guard lock(g_luaMutex);
        return g_hasManifestCodeFunc;
    }

    bool CallManifestFetchCode(uint64_t gid, uint64_t* outCode) {
        std::lock_guard lock(g_luaMutex);
        if (!g_hasManifestCodeFunc || !g_lua_state)
            return false;
        if (!outCode) return false;
        *outCode = 0;

        lua_getglobal(g_lua_state, "fetch_manifest_code");
        PushUInt64(g_lua_state, gid);

        if (lua_pcall(g_lua_state, 1, 1, 0) != LUA_OK) {
            LOG_MANIFEST_WARN("fetch_manifest_code({}) error: {}", gid,
                             lua_tostring(g_lua_state, -1));
            lua_pop(g_lua_state, 1);
            return false;
        }

        // nil → fallback to config url (normal, not an error)
        if (lua_isnil(g_lua_state, -1)) {
            LOG_MANIFEST_WARN("fetch_manifest_code({}) returned nil", gid);
            lua_pop(g_lua_state, 1);
            return false;
        }

        // The function must return a digit string (uint64 as decimal).
        // tonumber() loses precision for values > 2^53; string is safe.
        if (lua_isinteger(g_lua_state, -1)) {
            const lua_Integer value = lua_tointeger(g_lua_state, -1);
            if (value <= 0) {
                lua_pop(g_lua_state, 1);
                return false;
            }
            *outCode = static_cast<uint64_t>(value);
        } else if (lua_isstring(g_lua_state, -1)) {
            size_t length = 0;
            const char* s = lua_tolstring(g_lua_state, -1, &length);
            uint64_t parsed = 0;
            if (!ParseUInt64Decimal(std::string_view(s, length), &parsed) || parsed == 0) {
                LOG_MANIFEST_WARN("fetch_manifest_code({}) returned invalid numeric string '{}'",
                                 gid, s);
                lua_pop(g_lua_state, 1);
                return false;
            }
            *outCode = parsed;
        } else {
            LOG_MANIFEST_WARN("fetch_manifest_code({}) unexpected type (expected digit-string)",
                             gid);
            lua_pop(g_lua_state, 1);
            return false;
        }

        LOG_MANIFEST_INFO("fetch_manifest_code({}) = {}", gid, *outCode);
        lua_pop(g_lua_state, 1);
        return true;
    }

    bool HasManifestCodeFuncEx() {
        std::lock_guard lock(g_luaMutex);
        return g_hasManifestCodeFuncEx;
    }

    bool CallManifestFetchCodeEx(uint64_t app_id, uint64_t depot_id, uint64_t gid, uint64_t* outCode) {
        std::lock_guard lock(g_luaMutex);
        if (!g_hasManifestCodeFuncEx || !g_lua_state)
            return false;
        if (!outCode) return false;
        *outCode = 0;

        lua_getglobal(g_lua_state, "fetch_manifest_code_ex");
        PushUInt64(g_lua_state, app_id);
        PushUInt64(g_lua_state, depot_id);
        PushUInt64(g_lua_state, gid);

        if (lua_pcall(g_lua_state, 3, 1, 0) != LUA_OK) {
            LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) error: {}", app_id, depot_id, gid,
                             lua_tostring(g_lua_state, -1));
            lua_pop(g_lua_state, 1);
            return false;
        }

        if (lua_isnil(g_lua_state, -1)) {
            LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) returned nil", app_id, depot_id, gid);
            lua_pop(g_lua_state, 1);
            return false;
        }

        if (lua_isinteger(g_lua_state, -1)) {
            const lua_Integer value = lua_tointeger(g_lua_state, -1);
            if (value <= 0) {
                lua_pop(g_lua_state, 1);
                return false;
            }
            *outCode = static_cast<uint64_t>(value);
        } else if (lua_isstring(g_lua_state, -1)) {
            size_t length = 0;
            const char* s = lua_tolstring(g_lua_state, -1, &length);
            uint64_t parsed = 0;
            if (!ParseUInt64Decimal(std::string_view(s, length), &parsed) || parsed == 0) {
                LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) returned invalid numeric string '{}'",
                                 app_id, depot_id, gid, s);
                lua_pop(g_lua_state, 1);
                return false;
            }
            *outCode = parsed;
        } else {
            LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) unexpected type (expected digit-string)",
                             app_id, depot_id, gid);
            lua_pop(g_lua_state, 1);
            return false;
        }

        LOG_MANIFEST_INFO("fetch_manifest_code_ex({}, {}, {}) = {}", app_id, depot_id, gid, *outCode);
        lua_pop(g_lua_state, 1);
        return true;
    }

    // ── per-file unload ────────────────────────────────────────
    void UnloadFile(const std::string& filePath) {
        std::lock_guard luaLock(g_luaMutex);
        auto depotsIt = g_fileDepots.find(filePath);
        auto manifestIt = g_fileManifestOverrides.find(filePath);
        if (depotsIt == g_fileDepots.end() && manifestIt == g_fileManifestOverrides.end()) return;

        if (depotsIt != g_fileDepots.end()) {
            std::unique_lock lock(g_depotDataMutex);
            std::vector<AppId_t> affectedDepots;
            affectedDepots.reserve(depotsIt->second.size());
            for (AppId_t id : depotsIt->second) {
                auto refIt = g_depotRefCount.find(id);
                if (refIt == g_depotRefCount.end()) {
                    LOG_PACKAGE_WARN("UnloadFile: missing depot refcount for AppId {}", id);
                    continue;
                }
                LOG_PACKAGE_DEBUG("UnloadFile:Ref count for AppId {} is {}", id, refIt->second);
                if (refIt->second <= 1) {
                    g_depotRefCount.erase(refIt);
                    g_purchaseTime.erase(id);
                    g_pendingRemovals.push_back(id);
                } else {
                    --refIt->second;
                }
                affectedDepots.push_back(id);
            }

            LOG_PACKAGE_INFO("UnloadFile: removed {} depots from {}", depotsIt->second.size(), filePath);
            g_fileDepots.erase(depotsIt);
            g_fileDepotKeys.erase(filePath);
            g_fileParseSequence.erase(filePath);
            for (AppId_t id : affectedDepots)
                RebuildDepotKeyLocked(id);
        }

        if (manifestIt != g_fileManifestOverrides.end()) {
            std::vector<uint64_t> affectedDepots;
            affectedDepots.reserve(manifestIt->second.size());
            for (const auto& [depotId, _] : manifestIt->second) {
                affectedDepots.push_back(depotId);
            }

            g_fileManifestOverrides.erase(manifestIt);
            for (uint64_t depotId : affectedDepots) {
                RebuildManifestOverride(depotId);
            }

            LOG_MANIFEST_INFO("UnloadFile: removed {} manifest override(s) from {}", affectedDepots.size(), filePath);
        }

        g_fileParseSequence.erase(filePath);
        g_fileMtime.erase(filePath);
    }

    std::vector<AppId_t> TakePendingRemovals() {
        std::vector<AppId_t> result;
        result.swap(g_pendingRemovals);
        return result;
    }

    std::vector<AppId_t> TakePendingAdditions() {
        std::vector<AppId_t> result;
        result.swap(g_pendingAdditions);
        return result;
    }

    static std::vector<std::string> CollectLuaFiles(const std::string& directory) {
        std::vector<std::string> files;

        std::error_code ec;
        if (!std::filesystem::exists(directory, ec))
            std::filesystem::create_directories(directory, ec);
        if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec))
            return files;

        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".lua") continue;
            files.push_back(entry.path().string());
        }
        return files;
    }

    // ── single-file parser ──────────────────────────────────────
    void ParseFile(const std::string& filePath) {
        std::lock_guard luaLock(g_luaMutex);
        if (!Initialize()) return;

        // Remove old entries from this file before re-parsing.
        UnloadFile(filePath);
        g_currentFile = filePath;

        std::filesystem::path path(filePath);
        std::ifstream file(path);
        if (!file) {
            LOG_WARN("ParseFile: failed to open {}", path.filename().string());
            g_currentFile.clear();
            return;
        }
        g_fileParseSequence[filePath] = ++g_nextFileParseSequence;
        
        // Capture the file's last-modified time (unix epoch, seconds) so
        // lua_addappid can stamp it onto every appId this file contributes.
        // Portable conversion that does not require C++20 clock_cast.
        {
            std::error_code ec;
            auto ftime = std::filesystem::last_write_time(path, ec);
            uint32_t mtime = 0;
            if (!ec) {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());
                mtime = static_cast<uint32_t>(std::chrono::system_clock::to_time_t(sctp));
            }
            g_fileMtime[filePath] = mtime;
        }

        std::string chunk, line;
        int lineNo = 0;
        while (std::getline(file, line)) {
            ++lineNo;
            if (!chunk.empty()) chunk += '\n';
            chunk += line;

            lua_settop(g_lua_state, 0);
            int rc = luaL_loadstring(g_lua_state, chunk.c_str());
            if (rc == LUA_OK) {
                if (lua_pcall(g_lua_state, 0, 0, 0) != LUA_OK) {
                    const char* err = lua_tostring(g_lua_state, -1);
                    LOG_WARN("{}:{}: {}", path.filename().string(), lineNo,
                             err ? err : "unknown");
                }
                chunk.clear();
            } else if (rc == LUA_ERRSYNTAX) {
                lua_pop(g_lua_state, 1);
            } else {
                const char* err = lua_tostring(g_lua_state, -1);
                LOG_WARN("{}:{}: {}", path.filename().string(), lineNo, err ? err : "unknown");
                lua_pop(g_lua_state, 1);
                chunk.clear();
            }
        }
        if (!chunk.empty()) {
            LOG_WARN("{}: incomplete statement at end of file", path.filename().string());
        }

        // Check for manifest code functions after parsing.
        g_hasManifestCodeFunc = false;
        lua_getglobal(g_lua_state, "fetch_manifest_code");
        if (lua_isfunction(g_lua_state, -1)) {
            g_hasManifestCodeFunc = true;
            LOG_INFO("manifest.lua: fetch_manifest_code found");
        }
        lua_pop(g_lua_state, 1);

        g_hasManifestCodeFuncEx = false;
        lua_getglobal(g_lua_state, "fetch_manifest_code_ex");
        if (lua_isfunction(g_lua_state, -1)) {
            g_hasManifestCodeFuncEx = true;
            LOG_INFO("manifest.lua: fetch_manifest_code_ex found");
        }
        lua_pop(g_lua_state, 1);

        g_currentFile.clear();
    }

    // ── directory scanner ────────────────────────────────────────
    void ParseDirectory(const std::string& directory) {
        if (!Initialize()) return;

        for (const auto& filePath : CollectLuaFiles(directory)) {
            ParseFile(filePath);
        }

        // Initial parse — discard pending additions so NotifyLicenseChanged
        // only sees changes that happen after startup.
        g_pendingAdditions.clear();
    }

    void ReloadDirectories(const std::vector<std::string>& directories, bool clearPendingAdditions) {
        if (!Initialize()) return;

        std::unordered_set<std::string> activeFiles;
        std::vector<std::string> orderedFiles;

        for (const auto& directory : directories) {
            for (const auto& filePath : CollectLuaFiles(directory)) {
                if (activeFiles.insert(filePath).second) {
                    orderedFiles.push_back(filePath);
                }
            }
        }

        std::unordered_set<std::string> trackedSet;
        std::vector<std::string> trackedFiles;
        auto rememberTracked = [&](const std::string& filePath) {
            if (trackedSet.insert(filePath).second) {
                trackedFiles.push_back(filePath);
            }
        };

        for (const auto& [filePath, _] : g_fileDepots) {
            rememberTracked(filePath);
        }
        for (const auto& [filePath, _] : g_fileManifestOverrides) {
            rememberTracked(filePath);
        }

        for (const auto& filePath : trackedFiles) {
            if (!activeFiles.contains(filePath)) {
                UnloadFile(filePath);
            }
        }

        for (const auto& filePath : orderedFiles) {
            ParseFile(filePath);
        }

        if (clearPendingAdditions) {
            g_pendingAdditions.clear();
        }
    }

}
