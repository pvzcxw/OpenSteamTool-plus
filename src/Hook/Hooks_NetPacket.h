#pragma once

#include "Steam/Types.h"

#include <cstdint>

namespace Hooks_NetPacket {
    void Install();
    void Uninstall();
    bool RequestOwnershipTicket(AppId_t appId, uint32_t timeoutMs);
}
