#include "Hooks_Decryption.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "Utils/Tickets/AppTicket.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
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

    void HarvestAppticket(const std::string& name, const char* data, size_t size) {
        static constexpr std::string_view kPrefix = "apptickets\\";
        if (!name.starts_with(kPrefix)) return;

        const char* begin = name.data() + kPrefix.size();
        char* end = nullptr;
        const unsigned long value = std::strtoul(begin, &end, 10);
        if (value == 0 || value > UINT32_MAX || !end || *end != '\0') return;

        Hooks_Decryption::CacheAppOwnershipTicket(
            static_cast<AppId_t>(value), reinterpret_cast<const uint8_t*>(data), size);
    }

    HOOK_FUNC(ConfigStoreGetBinary, int32, void* pObject, EConfigStore eConfigStore, const char* KeyName, char* Key, uint32 KeySize) {
        if (eConfigStore == k_EConfigStoreUserLocal && pObject) {
            void* expected = nullptr;
            if (g_pConfigStoreLocal.compare_exchange_strong(
                    expected, pObject, std::memory_order_release, std::memory_order_relaxed)) {
                LOG_DECRYPTIONKEY_DEBUG("ConfigStoreGetBinary: captured local ConfigStore at {}", pObject);
            }
        }
        std::string name(KeyName);
        LOG_DECRYPTIONKEY_DEBUG("ConfigStore::GetBinary called for pObject={}, eConfigStore={}, KeyName='{}'", 
                                    pObject, static_cast<uint32>(eConfigStore), name);
        // Expected shape: ".../<DepotId>\DecryptionKey"
        if (size_t last = name.find("\\DecryptionKey"); last != std::string::npos) {
            if (size_t start = name.find_last_of("\\", last - 1); start != std::string::npos) {
                AppId_t depotId = std::stoul(name.substr(start + 1, last - start - 1));
                if (const auto& key = LuaConfig::GetDecryptionKey(depotId); !key.empty()) {
                    if (KeySize >= key.size()) {
                        LOG_DECRYPTIONKEY_INFO("Providing decryption key for depot {}: {}", depotId,
                                               spdlog::to_hex(key.data(), key.data() + key.size()));
                        memcpy(Key, key.data(), key.size());
                        return static_cast<int32>(key.size());
                    }
                    LOG_DECRYPTIONKEY_WARN("Decryption key for depot {} is too large ({} bytes) for buffer ({} bytes)",
                                            depotId, key.size(), KeySize);
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
