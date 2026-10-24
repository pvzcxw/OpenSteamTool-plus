#pragma once
#include "dllmain.h"

namespace Hooks_Decryption {
    // LoadDepotDecryptionKey hook: serves user-provided decryption keys for
    // depots configured via Lua.
    void Install();
    void Uninstall();

    std::vector<uint8_t> GetCacheAppOwnershipTicket(AppId_t appId);

    void CacheAppOwnershipTicket(AppId_t appId, const uint8_t* data, size_t size);
    std::vector<uint8_t> GetMemCachedAppOwnershipTicket(AppId_t appId);
    std::vector<uint8_t> GetAnyMemCachedAppOwnershipTicket();
    std::vector<uint8_t> WaitForCachedTicket(AppId_t appId, uint32_t timeoutMs);
}
