#include "Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/ManifestClient.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>

namespace Config {
namespace {

    struct Snapshot {
        std::string manifestProvider = "20770407";
        bool manifestFailover = true;
        ManifestTimeouts manifestTimeouts;
        LogLevel logLevel = LogLevel::Debug;
        std::string logDir;
        std::vector<std::string> luaPaths;
        std::string remoteUrlTemplate;
        std::string remoteOrder = "gh-proxy-first";
        bool statsEnableApi = true;
        InjectionSettings injection;
        CloudSettings cloud;
    };

    std::mutex g_mutex;
    bool g_loadedOnce = false;

    const char* ToString(LogLevel level) {
        switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        }
        return "???";
    }

    Snapshot MakeDefaultSnapshot(const std::string& configPath) {
        Snapshot snapshot;
        std::filesystem::path p(configPath);
        snapshot.logDir = (p.parent_path() / "opensteamtool").string();
        return snapshot;
    }

    void ApplySnapshot(const Snapshot& snapshot) {
        manifestTimeoutResolve = snapshot.manifestTimeouts.resolve;
        manifestTimeoutConnect = snapshot.manifestTimeouts.connect;
        manifestTimeoutSend    = snapshot.manifestTimeouts.send;
        manifestTimeoutRecv    = snapshot.manifestTimeouts.recv;
        manifestFailover       = snapshot.manifestFailover;
        logLevel               = snapshot.logLevel;
        logDir                 = snapshot.logDir;
        luaPaths               = snapshot.luaPaths;
        remoteUrlTemplate      = snapshot.remoteUrlTemplate;
        remoteOrder            = snapshot.remoteOrder;
        statsEnableApi         = snapshot.statsEnableApi;
        injectEnabled          = snapshot.injection.enabled;
        injectLibraryX86       = snapshot.injection.libraryX86;
        injectLibraryX64       = snapshot.injection.libraryX64;
        cloudEnabled           = snapshot.cloud.enabled;
        cloudLibrary           = snapshot.cloud.library;
    }

    void ApplyManifestProvider(const std::string& provider) {
        if (!ManifestClient::SetProvider(provider)) {
            LOG_WARN("Unknown manifest.url \"{}\", keeping default", provider);
            ManifestClient::SetProvider("20770407");
        }
    }

    static std::string NormalizeRemoteOrder(std::string value) {
        for (auto& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }

    // WinHTTP treats zero as an infinite timeout. Clamp values so one source
    // cannot exceed the shared manifest fetch budget.
    static void ReadTimeoutMs(const toml::table& manifest, const char* key, uint32_t& out) {
        const auto value = manifest[key].value<int64_t>();
        if (!value) return;
        constexpr int64_t kMin = 1;
        constexpr int64_t kMax = ManifestClient::kFetchBudgetMs;
        if (*value < kMin || *value > kMax)
            LOG_WARN("manifest.{} = {} is outside [{}, {}], clamping", key, *value, kMin, kMax);
        out = static_cast<uint32_t>(std::clamp<int64_t>(*value, kMin, kMax));
    }

    LoadResult ApplySnapshotLocked(const Snapshot& snapshot) {
        std::lock_guard lock(g_mutex);
        LoadResult result;
        result.luaPathsChanged = luaPaths != snapshot.luaPaths;
        ApplySnapshot(snapshot);
        g_loadedOnce = true;
        result.applied = true;
        return result;
    }

} // namespace

    LoadResult Load(const std::string& configPath) {
        Snapshot snapshot = MakeDefaultSnapshot(configPath);
        if (!std::filesystem::exists(configPath)) {
            LOG_INFO("Config file not found, using defaults");
            ApplyManifestProvider(snapshot.manifestProvider);
            ManifestClient::SetFailoverEnabled(snapshot.manifestFailover);
            LoadResult result = ApplySnapshotLocked(snapshot);
            LOG_INFO("Config loaded: manifest.url={} manifest.failover={} log.level={} lua.paths={} stats.enable_api={} remote.url_template={} remote.order={}",
                     ManifestClient::ActiveProviderName(),
                     ManifestClient::IsFailoverEnabled(),
                     ToString(GetLogLevel()),
                     (uint32_t)GetLuaPaths().size(),
                     GetStatsEnableApi(),
                     GetRemoteUrlTemplate().empty() ? "<default>" : GetRemoteUrlTemplate(),
                     GetRemoteOrder());
            return result;
        }

        try {
            auto tbl = toml::parse_file(configPath);

            // [manifest]
            if (auto manifest = tbl["manifest"].as_table()) {
                if (auto val = (*manifest)["url"].value<std::string>()) {
                    snapshot.manifestProvider = *val;
                }
                if (auto val = (*manifest)["failover"].value<bool>())
                    snapshot.manifestFailover = *val;
                ReadTimeoutMs(*manifest, "timeout_resolve_ms", snapshot.manifestTimeouts.resolve);
                ReadTimeoutMs(*manifest, "timeout_connect_ms", snapshot.manifestTimeouts.connect);
                ReadTimeoutMs(*manifest, "timeout_send_ms", snapshot.manifestTimeouts.send);
                ReadTimeoutMs(*manifest, "timeout_recv_ms", snapshot.manifestTimeouts.recv);
            }

            // [log]
            if (auto log = tbl["log"].as_table()) {
                if (auto val = (*log)["level"].value<std::string>()) {
                    if (*val == "trace")           snapshot.logLevel = LogLevel::Trace;
                    else if (*val == "debug")       snapshot.logLevel = LogLevel::Debug;
                    else if (*val == "info")        snapshot.logLevel = LogLevel::Info;
                    else if (*val == "warn")        snapshot.logLevel = LogLevel::Warn;
                    else if (*val == "error")       snapshot.logLevel = LogLevel::Error;
                }
            }

            // [lua]
            if (auto lua = tbl["lua"].as_table()) {
                if (auto arr = (*lua)["paths"].as_array()) {
                    for (auto& elem : *arr) {
                        if (auto str = elem.value<std::string>()) {
                            snapshot.luaPaths.push_back(*str);
                        }
                    }
                }
            }

            // [remote]
            if (auto remote = tbl["remote"].as_table()) {
                if (auto val = (*remote)["url_template"].value<std::string>()) {
                    snapshot.remoteUrlTemplate = *val;
                }
                if (auto val = (*remote)["order"].value<std::string>()) {
                    const std::string order = NormalizeRemoteOrder(*val);
                    if (order == "github-first" || order == "gh-proxy-first" ||
                        order == "jsdelivr-first") {
                        snapshot.remoteOrder = order;
                    } else {
                        LOG_WARN("Unknown remote.order '{}', keeping default '{}'", *val, snapshot.remoteOrder);
                    }
                }
            }

            // [stats]
            if (auto stats = tbl["stats"].as_table()) {
                if (auto val = (*stats)["enable_api"].value<bool>()) {
                    snapshot.statsEnableApi = *val;
                }
            }

            // [inject]
            if (auto inject = tbl["inject"].as_table()) {
                if (auto val = (*inject)["enabled"].value<bool>())
                    snapshot.injection.enabled = *val;
                if (auto val = (*inject)["library_x86"].value<std::string>())
                    snapshot.injection.libraryX86 = *val;
                if (auto val = (*inject)["library_x64"].value<std::string>())
                    snapshot.injection.libraryX64 = *val;
            }

            // [cloud]
            if (auto cloud = tbl["cloud"].as_table()) {
                if (auto val = (*cloud)["enabled"].value<bool>())
                    snapshot.cloud.enabled = *val;
                if (auto val = (*cloud)["library"].value<std::string>())
                    snapshot.cloud.library = *val;
            }

            ApplyManifestProvider(snapshot.manifestProvider);
            ManifestClient::SetFailoverEnabled(snapshot.manifestFailover);
            LoadResult result = ApplySnapshotLocked(snapshot);
            LOG_INFO("Config loaded: manifest.url={} manifest.failover={} log.level={} lua.paths={} stats.enable_api={} remote.url_template={} remote.order={}",
                     ManifestClient::ActiveProviderName(),
                     ManifestClient::IsFailoverEnabled(),
                     ToString(snapshot.logLevel),
                     (uint32_t)snapshot.luaPaths.size(),
                     snapshot.statsEnableApi,
                     snapshot.remoteUrlTemplate.empty() ? "<default>" : snapshot.remoteUrlTemplate,
                     snapshot.remoteOrder);
            return result;

        } catch (const toml::parse_error& e) {
            LOG_WARN("Config parse error: {}", e.what());
        } catch (...) {
            LOG_WARN("Config load failed");
        }
        bool shouldApplyDefault = false;
        {
            std::lock_guard lock(g_mutex);
            shouldApplyDefault = !g_loadedOnce;
        }
        if (shouldApplyDefault) {
            ApplyManifestProvider(snapshot.manifestProvider);
            ManifestClient::SetFailoverEnabled(snapshot.manifestFailover);
            std::lock_guard lock(g_mutex);
            const bool luaChanged = luaPaths != snapshot.luaPaths;
            ApplySnapshot(snapshot);
            g_loadedOnce = true;
            return {true, luaChanged};
        }
        return {};
    }

    ManifestTimeouts GetManifestTimeouts() {
        std::lock_guard lock(g_mutex);
        return {
            manifestTimeoutResolve,
            manifestTimeoutConnect,
            manifestTimeoutSend,
            manifestTimeoutRecv,
        };
    }

    LogLevel GetLogLevel() {
        std::lock_guard lock(g_mutex);
        return logLevel;
    }

    std::string GetLogDir() {
        std::lock_guard lock(g_mutex);
        return logDir;
    }

    std::vector<std::string> GetLuaPaths() {
        std::lock_guard lock(g_mutex);
        return luaPaths;
    }

    std::string GetRemoteUrlTemplate() {
        std::lock_guard lock(g_mutex);
        return remoteUrlTemplate;
    }

    std::string GetRemoteOrder() {
        std::lock_guard lock(g_mutex);
        return remoteOrder;
    }

    InjectionSettings GetInjectionSettings() {
        std::lock_guard lock(g_mutex);
        return {
            injectEnabled,
            injectLibraryX86,
            injectLibraryX64,
        };
    }

    bool GetStatsEnableApi() {
        std::lock_guard lock(g_mutex);
        return statsEnableApi;
    }

    CloudSettings GetCloudSettings() {
        std::lock_guard lock(g_mutex);
        return {
            cloudEnabled,
            cloudLibrary,
        };
    }

}
