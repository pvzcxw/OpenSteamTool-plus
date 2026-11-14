#include "Hooks_Decryption.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "Utils/Tickets/AppTicket.h"
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include "Utils/Config/LuaConfig.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {

    std::atomic<void*> g_pConfigStoreLocal{nullptr};

    // Tickets read by Steam itself are useful forge templates when a fresh
    // localconfig has no apptickets\7 entry. Entries are scoped to one SteamID.
    std::mutex g_TicketMtx;
    std::condition_variable g_TicketCv;
    std::unordered_map<AppId_t, std::vector<uint8_t>> g_Tickets;
    uint64_t g_TicketSteamId = 0;

    bool ParseAppId(std::string_view text, AppId_t* outAppId) {
        if (!outAppId || text.empty()) return false;
        uint32_t value = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || ptr != text.data() + text.size() || value == 0)
            return false;
        *outAppId = value;
        return true;
    }

    void HarvestAppticket(const std::string& name, const char* data, size_t size) {
        static constexpr std::string_view kPrefix = "apptickets\\";
        if (!name.starts_with(kPrefix)) return;

        AppId_t appId = 0;
        if (!ParseAppId(std::string_view(name).substr(kPrefix.size()), &appId)) return;

        Hooks_Decryption::CacheAppOwnershipTicket(
            appId, reinterpret_cast<const uint8_t*>(data), size);
    }

    HOOK_FUNC(ConfigStoreGetBinary, int32, void* pObject, EConfigStore eConfigStore, const char* KeyName, char* Key, uint32 KeySize) {
        if (eConfigStore == k_EConfigStoreUserLocal && pObject) {
            void* expected = nullptr;
            if (g_pConfigStoreLocal.compare_exchange_strong(
                    expected, pObject, std::memory_order_release, std::memory_order_relaxed)) {
                LOG_DECRYPTIONKEY_DEBUG("ConfigStoreGetBinary: captured local ConfigStore at {}", pObject);
            }
        }
        if (!KeyName)
            return oConfigStoreGetBinary(pObject, eConfigStore, KeyName, Key, KeySize);
        std::string name(KeyName);
        LOG_DECRYPTIONKEY_DEBUG("ConfigStore::GetBinary called for pObject={}, eConfigStore={}, KeyName='{}'", 
                                    pObject, static_cast<uint32>(eConfigStore), name);

        // 票据读取。带壳的游戏启动时会在这里取所有权票据，拿不到就过不了壳的校验。
        // 假入库的游戏本地没有票，用一张真实票据改出目标 appid 补上，
        // 签名段保持原样，壳按老版本的解析方式能对得上。
        static constexpr std::string_view kTicketPrefix = "apptickets\\";
        if (eConfigStore == k_EConfigStoreUserLocal &&
            name.rfind(kTicketPrefix, 0) == 0) {
            const int32 got = oConfigStoreGetBinary(pObject, eConfigStore, KeyName, Key, KeySize);
            if (got > 0) {
                if (Key && static_cast<uint32>(got) <= KeySize)
                    HarvestAppticket(name, Key, static_cast<size_t>(got));
                return got;
            }
            AppId_t ticketAppId = 0;
            const std::string_view suffix(name.data() + kTicketPrefix.size(),
                                          name.size() - kTicketPrefix.size());
            if (!ParseAppId(suffix, &ticketAppId)) return got;
            LOG_DECRYPTIONKEY_DEBUG("ConfigStore appticket '{}': ret={}", name, got);

            if (Key && KeySize != 0 &&
                LuaConfig::HasDepot(ticketAppId, false)) {
                AppTicket::AppOwnershipTicket forged{};
                if (AppTicket::GetAppOwnershipTicket(ticketAppId, forged,
                        AppTicket::AppTicketSource::ForgeOnly)
                    && !forged.data.empty()
                    && forged.data.size() <= KeySize
                    && forged.data.size() <= static_cast<size_t>(std::numeric_limits<int32>::max())) {
                    // 顺带把主数据区里的 appid 也改成目标值。插入法只在签名前
                    // 多塞了一份，按常规偏移读到的还是 donor 的值，两边对不上。
                    constexpr size_t kAppIdEnd = AppTicket::kAppTicketAppIdOffset + sizeof(AppId_t);
                    if (forged.data.size() < kAppIdEnd) {
                        LOG_DECRYPTIONKEY_WARN("ConfigStore appticket '{}': forged ticket is too short to contain app id",
                                               name);
                        return got;
                    }
                    memcpy(forged.data.data() + AppTicket::kAppTicketAppIdOffset,
                           &ticketAppId, sizeof(AppId_t));
                    memcpy(Key, forged.data.data(), forged.data.size());
                    LOG_DECRYPTIONKEY_DEBUG("ConfigStore appticket '{}': provided forged ticket ({} bytes)",
                                            name, forged.data.size());
                    return static_cast<int32>(forged.data.size());
                }
                LOG_DECRYPTIONKEY_WARN("ConfigStore appticket '{}': no donor ticket available to forge", name);
            }
            return got;
        }

        // Expected shape: ".../<DepotId>\DecryptionKey"
        static constexpr std::string_view kDecryptionSuffix = "\\DecryptionKey";
        if (size_t last = name.find(kDecryptionSuffix); last != std::string::npos && last > 0 &&
            last + kDecryptionSuffix.size() == name.size()) {
            if (size_t start = name.find_last_of("\\", last - 1); start != std::string::npos) {
                AppId_t depotId = 0;
                if (start + 1 < last &&
                    ParseAppId(std::string_view(name.data() + start + 1, last - start - 1), &depotId)) {
                    if (const auto key = LuaConfig::GetDecryptionKey(depotId); !key.empty()) {
                        if (Key && KeySize >= key.size()) {
                            LOG_DECRYPTIONKEY_INFO("Providing decryption key for depot {} ({} bytes)",
                                                   depotId, key.size());
                            memcpy(Key, key.data(), key.size());
                            return static_cast<int32>(key.size());
                        }
                        LOG_DECRYPTIONKEY_WARN("Decryption key for depot {} is too large or output buffer is null ({} bytes, buffer {})",
                                               depotId, key.size(), KeySize);
                    }
                }
            }
        }
        const int32 result = oConfigStoreGetBinary(pObject, eConfigStore, KeyName, Key, KeySize);
        if (result > 0 && Key && static_cast<uint32>(result) <= KeySize) {
            HarvestAppticket(name, Key, static_cast<size_t>(result));
        }
        return result;
    }

    std::vector<uint8_t> ReadConfigStoreLocalBinary(const std::string& keyName) {
        void* pConfigStore = g_pConfigStoreLocal.load(std::memory_order_acquire);
        if (!pConfigStore || !oConfigStoreGetBinary) {
            LOG_DECRYPTIONKEY_WARN("GetConfigStoreLocalBinary: ConfigStoreGetBinary not ready, cannot get binary value");
            return {};
        }

        std::vector<uint8_t> value(1024);
        int32 result = oConfigStoreGetBinary(pConfigStore, k_EConfigStoreUserLocal,
                                             keyName.c_str(),
                                             reinterpret_cast<char*>(value.data()),
                                             static_cast<uint32>(value.size()));
        if (result <= 0) {
            LOG_DECRYPTIONKEY_DEBUG("GetConfigStoreLocalBinary: failed to read KeyName='{}'", keyName);
            return {};
        }

        value.resize(result);
        LOG_DECRYPTIONKEY_DEBUG("GetConfigStoreLocalBinary: got value for KeyName='{}' ({} bytes)",
                                keyName, value.size());
        return value;
    }
}

namespace Hooks_Decryption {
    void Install() {
        HOOK_BEGIN();
        INSTALL_HOOK_C(ConfigStoreGetBinary);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(ConfigStoreGetBinary);
        UNHOOK_END();
    }

    std::vector<uint8_t> GetCacheAppOwnershipTicket(AppId_t appId) {
        std::vector<uint8_t> ticket = ReadConfigStoreLocalBinary(std::format("apptickets\\{}", appId));
        if (ticket.empty()) {
            LOG_DECRYPTIONKEY_DEBUG("no cached ticket for AppId {}", appId);
            return ticket;
        }
        LOG_DECRYPTIONKEY_DEBUG("got cached ticket for AppId {} ({} bytes)", appId, ticket.size());
        return ticket;
    }

    void CacheAppOwnershipTicket(AppId_t appId, const uint8_t* data, size_t size) {
        if (!data || appId == 0 || size <= AppTicket::kAppTicketSignatureSize) return;

        std::vector<uint8_t> bytes(data, data + size);
        uint64_t steamId = 0;
        if (bytes.size() < AppTicket::kAppTicketSteamIdOffset + sizeof(steamId)) return;
        std::memcpy(&steamId, bytes.data() + AppTicket::kAppTicketSteamIdOffset, sizeof(steamId));
        if (steamId == 0) return;

        bool isNew = false;
        {
            std::lock_guard<std::mutex> lock(g_TicketMtx);
            if (g_TicketSteamId != 0 && g_TicketSteamId != steamId) {
                LOG_DECRYPTIONKEY_INFO("ownership ticket cache flushed: SteamID {:017} -> {:017}",
                                       g_TicketSteamId, steamId);
                g_Tickets.clear();
            }
            g_TicketSteamId = steamId;
            isNew = g_Tickets.find(appId) == g_Tickets.end();
            g_Tickets[appId] = std::move(bytes);
        }
        if (isNew) {
            LOG_DECRYPTIONKEY_INFO("harvested ownership ticket for AppId {} ({} bytes, SteamID {:017})",
                                   appId, size, steamId);
        }
        g_TicketCv.notify_all();
    }

    std::vector<uint8_t> GetMemCachedAppOwnershipTicket(AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_TicketMtx);
        const auto it = g_Tickets.find(appId);
        return it == g_Tickets.end() ? std::vector<uint8_t>{} : it->second;
    }

    std::vector<uint8_t> GetAnyMemCachedAppOwnershipTicket() {
        std::lock_guard<std::mutex> lock(g_TicketMtx);
        return g_Tickets.empty() ? std::vector<uint8_t>{} : g_Tickets.begin()->second;
    }

    std::vector<uint8_t> WaitForCachedTicket(AppId_t appId, uint32_t timeoutMs) {
        std::unique_lock<std::mutex> lock(g_TicketMtx);
        const bool have = g_TicketCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [appId] {
            const auto it = g_Tickets.find(appId);
            return it != g_Tickets.end() && !it->second.empty();
        });
        if (!have) return {};
        return g_Tickets.at(appId);
    }
}
