#pragma once

#include "Pipe/PipeTypes.h"

namespace PipeManager::DenuvoAuth {

    // Per-handshake entry point: runs the one-time Denuvo detection (cached per
    // process) and arms the adaptive authorization time window.
    void Apply(const PipeContext& ctx);

    // True only while the pipe belongs to a Denuvo process and is currently
    // within the active authorization window (startup grace period or ticket lease).
    [[nodiscard]] bool IsAuthorizedPipe(const CPipeClient* pipe);

    // True if the pipe belongs to a process detected or forced as Denuvo.
    [[nodiscard]] bool IsDenuvoPipe(const CPipeClient* pipe);

    // Returns the AppId tracked for this pipe, or k_uAppIdInvalid if unknown.
    [[nodiscard]] AppId_t GetAuthorizedAppId(const CPipeClient* pipe);

    // Refreshes the authorization window when an ownership ticket is requested (Scheme 1: 300ms micro-pulse).
    void OnOwnershipTicketRequested(const CPipeClient* pipe, AppId_t appId = k_uAppIdInvalid);

    // Signals that an encrypted ticket was asynchronously requested (binds AppId early with 0ms identity pollution).
    void OnEncryptedTicketRequested(const CPipeClient* pipe, AppId_t appId = k_uAppIdInvalid);

    // Triggered when an encrypted ticket is delivered to the game: arms the activation delivery pulse (2500ms).
    void OnEncryptedTicketDelivered(const CPipeClient* pipe, AppId_t appId = k_uAppIdInvalid);

} // namespace PipeManager::DenuvoAuth
