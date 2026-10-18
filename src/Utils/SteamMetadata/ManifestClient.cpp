#include "ManifestClient.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string_view>

namespace ManifestClient {

    // ── parsers ────────────────────────────────────────────────────
    using Parser = bool (*)(std::string_view body, uint64_t* out);

    static bool ParsePlainUint(std::string_view body, uint64_t* out) {
        uint64_t code = 0;
        auto [_, ec] = std::from_chars(body.data(), body.data() + body.size(), code);
        if (ec != std::errc{}) return false;
        *out = code;
        return true;
    }

    static bool ParseSteamRunJson(std::string_view body, uint64_t* out) {
        size_t key = body.find("\"content\"");
        if (key == std::string_view::npos) return false;
        size_t q1 = body.find('"', key + 9);
        if (q1 == std::string_view::npos) return false;
        size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string_view::npos) return false;
        return ParsePlainUint(body.substr(q1 + 1, q2 - q1 - 1), out);
    }

    // ── provider table ────────────────────────────────────────────
    //
    // Adding a new provider: add one row to kProviders below.
    // host / port / tls / path are all derived from the URL template
    // by Make() at compile time.
    // 表顺序即顺位回退顺序，首行须为默认首选源。

    struct Provider {
        std::string_view name;          // matches [manifest] url = "..."
        const char*      urlTemplate;   // full literal with one %llu (gid-only)
                                        // or two %llu (depotId, gid) when needsDepot
        Parser           parse;
        bool             needsDepot;
        const wchar_t*   headers;        // extra request headers (e.g. UA gate), or nullptr
    };

    consteval Provider Make(std::string_view name, const char* url, Parser parse,
                            bool needsDepot = false, const wchar_t* headers = nullptr) {
        return {name, url, parse, needsDepot, headers};
    }

    static constexpr Provider kProviders[] = {
        Make("20770407",    "https://20770407.xyz/manifest/%llu/%llu",   ParsePlainUint, true),
        Make("SDM",           "https://steamapi.993499094.xyz/manifest/%llu/%llu", ParsePlainUint, true),
        Make("manifestdex",   "https://manifest.manifestdex.com/%llu",   ParsePlainUint, false,
             L"User-Agent: ManifestDeX/1.0"),
        Make("wudrm",         "http://gmrc.wudrm.com/manifest/%llu",     ParsePlainUint),
        Make("steamrun",      "https://manifest.steam.run/api/manifest/%llu", ParseSteamRunJson),
        Make("opensteamtool", "https://manifest.opensteamtool.com/%llu", ParsePlainUint),
    };

    static constexpr size_t kProviderCount = std::size(kProviders);

    // ── provider state ────────────────────────────────────────────
    //
    // 该锁仅保护下标与冷却表等纯内存状态，持锁期间不做
    // HTTP 或 Lua 调用，避免挂掉的上游阻塞并发 depot。

    static std::mutex g_stateMutex;

    // 配置指定的首选源下标，回退永远从它开始，成功不会改写它。
    static size_t g_active = 0;                            // 20770407

    // 各源失败截止点；未来时间之前该源直接跳过，不再支付超时。
    static std::chrono::steady_clock::time_point g_deadUntil[kProviderCount];

    // 各源成功计数：并发取码时失败可能晚于成功到达，用它比对能
    // 识别过期失败，避免把刚被证明活着的源误判进冷却。
    static uint64_t g_okSeq[kProviderCount] = {};

    // 顺位回退总开关，对应 [manifest] failover。
    static bool g_failoverEnabled = true;

    static size_t ActiveIndex() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_active;
    }

    static bool FailoverEnabled() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_failoverEnabled;
    }

    static bool IsCoolingDown(size_t i, std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_deadUntil[i] > now;
    }

    // 请求发出前采样，失败回写时比对。
    static uint64_t OkSeq(size_t i) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_okSeq[i];
    }

    static void MarkFailed(size_t i, std::chrono::steady_clock::time_point now, uint64_t seq) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        // 采样之后已有成功到达，这次失败描述的是过期状态，直接丢弃。
        if (g_okSeq[i] != seq) {
            LOG_MANIFEST_DEBUG("Manifest {} failure ignored (superseded by a success)",
                               kProviders[i].name);
            return;
        }
        g_deadUntil[i] = now + std::chrono::milliseconds(kProviderCooldownMs);
    }

    // 记录该源存活并清除其冷却，不改写首选下标。
    static void RecordSuccess(size_t i) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        ++g_okSeq[i];
        g_deadUntil[i] = {};
    }

    bool SetProvider(std::string_view name) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (size_t i = 0; i < kProviderCount; ++i)
            if (kProviders[i].name == name) {
                // 同名重选视为无操作：toml 任意改动都会触发全量重载，
                // 在这里清冷却会把已知挂掉的源重新送回首发位置。
                if (i == g_active) return true;
                g_active = i;
                // 只为新选中的源清除冷却并递增序号，其余源的冷却结论保留；
                // 在途失败不覆盖本次切换。
                g_deadUntil[i] = {};
                ++g_okSeq[i];
                return true;
            }
        return false;
    }

    void SetFailoverEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_failoverEnabled == enabled) return;
        g_failoverEnabled = enabled;
        // 开关翻转后旧冷却结论不再可信，直接清掉从干净状态起算。
        for (auto& t : g_deadUntil) t = {};
    }

    bool IsFailoverEnabled() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_failoverEnabled;
    }

    const char* ActiveProviderName() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return kProviders[g_active].name.data();
    }

    // ── request ───────────────────────────────────────────────────

    void Shutdown() {
    }

    // ── fetch ─────────────────────────────────────────────────────

    // 对单个源发起一次请求；超时由调用方按预算钳制好再传入。
    static bool FetchFrom(const Provider& p, uint64_t gid, AppId_t depotId, uint64_t* outCode,
                          uint32_t resolveMs, uint32_t connectMs, uint32_t sendMs, uint32_t recvMs) {
        char urlLog[256];
        if (p.needsDepot) {
            if (!depotId) return false;
            std::snprintf(urlLog, sizeof(urlLog), p.urlTemplate,
                          static_cast<unsigned long long>(depotId),
                          static_cast<unsigned long long>(gid));
        } else {
            std::snprintf(urlLog, sizeof(urlLog), p.urlTemplate,
                          static_cast<unsigned long long>(gid));
        }

        auto r = OSTPlatform::Http::Execute(
            L"GET",
            urlLog,
            nullptr,
            0,
            p.headers,
            resolveMs,
            connectMs,
            sendMs,
            recvMs);

        LOG_MANIFEST_INFO("Manifest {} status={} depot={} gid={}", p.name, r.status, depotId, gid);

        if (!r.ok || r.status != 200) return false;
        return p.parse(r.body, outCode);
    }

    // 开关关闭时仅请求首选源，不回退。
    static bool FetchActive(uint64_t gid, uint64_t* outCode, AppId_t depotId = 0) {
        const Provider& p = kProviders[ActiveIndex()];
        const Config::ManifestTimeouts timeouts = Config::GetManifestTimeouts();
        return FetchFrom(p, gid, depotId, outCode,
                         timeouts.resolve, timeouts.connect, timeouts.send, timeouts.recv);
    }

    // 从首选源起算环形遍历全表：冷却中跳过，缺 depotId 时跳过
    // needsDepot 源（不记失败），预算耗尽即停；全表冷却则直接返回。
    static bool FetchWithFailover(uint64_t gid, uint64_t* outCode, AppId_t depotId = 0) {
        const Config::ManifestTimeouts timeouts = Config::GetManifestTimeouts();
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(kFetchBudgetMs);

        // 起点只快照一次，并发取码中途改首选不影响本次遍历顺序。
        const size_t start = ActiveIndex();

        size_t tried = 0;
        for (size_t k = 0; k < kProviderCount; ++k) {
            const size_t i = (start + k) % kProviderCount;
            const Provider& p = kProviders[i];

            const auto now = std::chrono::steady_clock::now();
            if (IsCoolingDown(i, now)) {
                LOG_MANIFEST_DEBUG("Manifest {} skipped (cooldown)", p.name);
                continue;
            }
            if (p.needsDepot && !depotId) {
                LOG_MANIFEST_DEBUG("Manifest {} skipped (no depot id)", p.name);
                continue;
            }

            // 请求发出前采样成功计数，失败回写时比对，过滤过期失败。
            const uint64_t seq = OkSeq(i);

            // 单次尝试不得超过剩余预算：hook 只等到预算+1秒，
            // 更慢的应答对 Steam 已无意义，只会拖住下一个 depot。
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();
            if (left <= 0) {
                LOG_MANIFEST_WARN("Manifest gid={}: {}ms budget exhausted after {} attempt(s)",
                                  gid, kFetchBudgetMs, tried);
                break;
            }
            // 上下钳制：不超剩余预算，且不低于 1ms——WinHTTP 会把
            // 零超时读成无限等待，单次尝试就会跑穿 hook 的等待窗口。
            const auto cap = [left](uint32_t ms) {
                return static_cast<uint32_t>(
                    std::max<int64_t>(1, std::min<int64_t>(ms, left)));
            };

            const bool ok = FetchFrom(p, gid, depotId, outCode,
                                      cap(timeouts.resolve), cap(timeouts.connect),
                                      cap(timeouts.send), cap(timeouts.recv));
            ++tried;

            if (ok) {
                RecordSuccess(i);
                return true;
            }

            MarkFailed(i, std::chrono::steady_clock::now(), seq);
        }

        if (tried == 0)
            LOG_MANIFEST_WARN("Manifest gid={}: every provider is in cooldown", gid);
        return false;
    }

    // ── public ────────────────────────────────────────────────────

    // LuaConfig 只暴露一个共享 lua_State 且内部不加锁，并发取码必须
    // 在这里串行；锁只覆盖 Lua 段，网络 I/O 开始前释放。
    static std::mutex g_luaMutex;

    // Lua 产出取码即返回 true，调用方否则继续走 HTTP；独立成函数
    // 是为了让锁在发包前释放。
    static bool TryLua(uint64_t manifestGid, uint64_t* outRequestCode,
                       AppId_t appId, AppId_t depotId)
    {
        std::lock_guard<std::mutex> lock(g_luaMutex);

        if (appId && depotId && LuaConfig::HasManifestCodeFuncEx()) {
            if (LuaConfig::CallManifestFetchCodeEx(appId, depotId, manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via fetch_manifest_code_ex", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} fetch_manifest_code_ex returned nil, trying fetch_manifest_code", manifestGid);
        }

        if (LuaConfig::HasManifestCodeFunc()) {
            if (LuaConfig::CallManifestFetchCode(manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via manifest.lua", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} lua returned nil, falling back to config", manifestGid);
        }

        return false;
    }

    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId, AppId_t depotId)
    {
        if (TryLua(manifestGid, outRequestCode, appId, depotId))
            return true;

        if (FailoverEnabled())
            return FetchWithFailover(manifestGid, outRequestCode, depotId);

        return FetchActive(manifestGid, outRequestCode, depotId);
    }
}
