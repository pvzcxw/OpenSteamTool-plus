#pragma once
#include "Steam/Types.h"
#include <string_view>

// ─────────────────────────────────────────────────────────────────
//  ManifestClient — HTTP client for depot manifest request codes.
//  Provider table is internal (see kProviders in ManifestClient.cpp);
//  adding a new provider only requires one row there.
//
//  从配置的首选源起算顺位回退：失败尝试下一个，刚失败的源在
//  冷却期内直接跳过；首选源恒等于配置值，成功不改写。
//
//  Thread-safe — provider state is guarded by a short-held mutex that
//  never spans I/O; HTTP requests run concurrently.
// ─────────────────────────────────────────────────────────────────
namespace ManifestClient {

    // 单次取码总预算：全部回退尝试共享，hook 等待比它多 1 秒。
    constexpr uint32_t kFetchBudgetMs = 15000;

    // 单源失败后的跳过时长，避免每个 depot 重复支付超时。
    constexpr uint32_t kProviderCooldownMs = 60000;

    // Select the active provider by its string name (matches kProviders[i].name).
    // Returns false if no provider matches; the previous selection is kept.
    bool SetProvider(std::string_view name);

    // Name of the currently active provider (for logging / diagnostics).
    const char* ActiveProviderName();

    // 顺位回退总开关（对应 [manifest] failover），热加载实时生效。
    void SetFailoverEnabled(bool enabled);
    bool IsFailoverEnabled();

    // Resolve a manifest GID to its request code. Tries Lua first
    // (fetch_manifest_code_ex, then fetch_manifest_code), then the
    // configured provider (with failover to the rest of the table when
    // enabled). Returns true and sets *outRequestCode on success.
    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId = 0, AppId_t depotId = 0);

    // Tear down the cached WinHTTP connection (call at unload).
    void Shutdown();
}
