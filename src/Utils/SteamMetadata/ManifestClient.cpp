#include "ManifestClient.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <mutex>
#include <string_view>

namespace ManifestClient {

    // ── parsers ────────────────────────────────────────────────────
    using Parser = bool (*)(std::string_view body, uint64_t* out);

    static std::string_view TrimAsciiWhitespace(std::string_view value) {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                                  value.front() == '\r' || value.front() == '\n'))
            value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                                  value.back() == '\r' || value.back() == '\n'))
            value.remove_suffix(1);
        return value;
    }

    static bool ParsePlainUint(std::string_view body, uint64_t* out) {
        if (!out) return false;
        body = TrimAsciiWhitespace(body);
        if (body.empty()) return false;
        uint64_t code = 0;
        const auto [ptr, ec] = std::from_chars(body.data(), body.data() + body.size(), code);
        if (ec != std::errc{} || ptr != body.data() + body.size() || code == 0)
            return false;
        *out = code;
        return true;
    }

    static bool ParseSteamRunJson(std::string_view body, uint64_t* out) {
        size_t key = body.find("\"content\"");
        if (key == std::string_view::npos) return false;
        size_t pos = key + 9;
        const auto skipWhitespace = [&]() {
            while (pos < body.size() &&
                   (body[pos] == ' ' || body[pos] == '\t' ||
                    body[pos] == '\r' || body[pos] == '\n')) {
                ++pos;
            }
        };
        skipWhitespace();
        if (pos >= body.size() || body[pos++] != ':') return false;
        skipWhitespace();
        if (pos >= body.size() || body[pos++] != '"') return false;

        const size_t valueStart = pos;
        while (pos < body.size() && body[pos] >= '0' && body[pos] <= '9') ++pos;
        if (pos == valueStart || pos >= body.size() || body[pos] != '"') return false;
        const size_t valueEnd = pos;
        ++pos;
        skipWhitespace();
        if (pos < body.size() && body[pos] != ',' && body[pos] != '}') return false;
        return ParsePlainUint(body.substr(valueStart, valueEnd - valueStart), out);
    }

    // ── provider table ────────────────────────────────────────────
    //
    // Adding a new provider: add one row to kProviders below.
    // host / port / tls / path are all derived from the URL template
    // by Make() at compile time.

    struct Provider {
        std::string_view name;          // matches [manifest] url = "..."
        const char*      urlTemplate;   // one %llu for gid, or two for depotId and gid
        const char*      urlTemplateAppDepot; // appId, depotId, gid when supported
        Parser           parse;
        bool             needsDepot;
        const wchar_t*   headers;        // optional request headers
    };

    consteval Provider Make(std::string_view name, const char* url, Parser parse,
                            bool needsDepot = false, const wchar_t* headers = nullptr,
                            const char* urlAppDepot = nullptr) {
        return {name, url, urlAppDepot, parse, needsDepot, headers};
    }

    static constexpr Provider kProviders[] = {
        Make("opensteamtool", "https://manifest.opensteamtool.com/%llu",       ParsePlainUint,
             false, nullptr, "https://manifest.opensteamtool.com/%llu/%llu/%llu"),
        Make("wudrm",         "http://gmrc.wudrm.com/manifest/%llu",           ParsePlainUint),
        Make("steamrun",      "https://manifest.steam.run/api/manifest/%llu",  ParseSteamRunJson),
        Make("20770407",      "https://20770407.xyz/manifest/%llu/%llu",       ParsePlainUint, true),
        Make("SDM",           "https://steamapi.993499094.xyz/manifest/%llu/%llu", ParsePlainUint, true),
        Make("manifestdex",   "https://manifest.manifestdex.com/%llu",         ParsePlainUint,
             false, L"User-Agent: ManifestDeX/1.0"),
    };

    static const Provider* g_active = &kProviders[0];   // opensteamtool
    static std::mutex      g_mutex;

    bool SetProvider(std::string_view name) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& p : kProviders)
            if (p.name == name) { 
                g_active = &p; 
                return true; 
            }
        return false;
    }

    const char* ActiveProviderName() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_active->name.data(); 
    }

    // ── request ───────────────────────────────────────────────────

    void Shutdown() {
        std::lock_guard<std::mutex> lock(g_mutex);
    }

    // ── fetch ─────────────────────────────────────────────────────

    static bool FetchActive(uint64_t gid, uint64_t* outCode, AppId_t appId, AppId_t depotId) {
        const Provider& p = *g_active;
        const Config::ManifestTimeouts timeouts = Config::GetManifestTimeouts();

        if (p.needsDepot && !depotId) return false;

        char urlLog[256];
        const bool appDepotAware = p.urlTemplateAppDepot && depotId;
        if (appDepotAware) {
            std::snprintf(urlLog, sizeof(urlLog), p.urlTemplateAppDepot,
                          static_cast<unsigned long long>(appId),
                          static_cast<unsigned long long>(depotId),
                          static_cast<unsigned long long>(gid));
        } else if (p.needsDepot) {
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
            timeouts.resolve,
            timeouts.connect,
            timeouts.send,
            timeouts.recv);

        LOG_MANIFEST_INFO("Manifest {} status={} depot={} gid={}", p.name, r.status, depotId, gid);

        if (!r.ok || r.status != 200) return false;
        return p.parse(r.body, outCode);
    }

    // ── public ────────────────────────────────────────────────────

    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId, AppId_t depotId)
    {
        if (!outRequestCode) return false;
        *outRequestCode = 0;
        if (manifestGid == 0) return false;
        std::lock_guard<std::mutex> lock(g_mutex);

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

        return FetchActive(manifestGid, outRequestCode, appId, depotId);
    }
}
