#include "RemoteToml.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

namespace RemoteToml {

namespace {
    // 签名源：hazre/steam-monitor（定时采集 stable+beta，文件更全）。
    // 与 OpenSteam001 版布局差异：channel 不是分支而是 main 下的目录，
    // 因此分支固定 main，{channel} 走路径段——占位符集合不变，模板机制兼容。
    constexpr const char* kGithubTemplate =
        "https://raw.githubusercontent.com/hazre/steam-monitor/"
        "main/{channel}/{component}/{sha256}.toml";
    // gh-proxy 套在 raw 直连外面：对大仓库的 raw 文件比 jsDelivr 稳定，
    // 失败才回退 GitHub 直连保底。
    constexpr const char* kGhProxyTemplate =
        "https://gh-proxy.com/https://raw.githubusercontent.com/hazre/steam-monitor/"
        "main/{channel}/{component}/{sha256}.toml";

    static bool HasPlaceholder(std::string_view text, std::string_view placeholder)
    {
        return text.find(placeholder) != std::string_view::npos;
    }

    static bool IsValidTemplate(std::string_view urlTemplate)
    {
        return HasPlaceholder(urlTemplate, "{channel}") &&
               HasPlaceholder(urlTemplate, "{component}") &&
               HasPlaceholder(urlTemplate, "{sha256}");
    }

    static void ReplaceAll(std::string& text,
                           std::string_view from,
                           std::string_view to)
    {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    static std::string ExpandTemplate(std::string urlTemplate,
                                      const Request& request,
                                      std::string_view sha256)
    {
        ReplaceAll(urlTemplate, "{channel}", request.channel);
        ReplaceAll(urlTemplate, "{component}", request.component);
        ReplaceAll(urlTemplate, "{sha256}", sha256);
        return urlTemplate;
    }

    static std::vector<std::string> BuildUrlTemplates()
    {
        const std::string remoteUrlTemplate = Config::GetRemoteUrlTemplate();
        if (!remoteUrlTemplate.empty()) {
            if (!IsValidTemplate(remoteUrlTemplate)) {
                LOG_WARN("RemoteToml: remote.url_template must contain "
                         "{channel}, {component}, and {sha256}; remote fetch disabled");
                return {};
            }

            return { remoteUrlTemplate };
        }

        // 默认优先请求 gh-proxy 镜像，失败再回退 GitHub 直连
        if (Config::GetRemoteOrder() == "github-first")
            return { kGithubTemplate, kGhProxyTemplate };
        return { kGhProxyTemplate, kGithubTemplate };
    }
} // namespace

Result Fetch(const Request& request)
{
    namespace fs = std::filesystem;
    Result out;

    // 1. SHA-256 of the DLL.
    const auto hashStart = std::chrono::steady_clock::now();
    out.sha256 = SteamDiagnostics::Sha256Of(request.dllPath);
    const auto hashMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - hashStart).count();

    if (out.sha256.empty()) {
        LOG_WARN("RemoteToml({}/{}): Sha256OfFile failed for {} ({} ms)",
                 request.channel, request.component, request.dllPath, hashMs);
        return out;
    }
    LOG_INFO("RemoteToml({}/{}): sha256 = {} ({} ms)",
             request.channel, request.component, out.sha256, hashMs);

    // 2. Cache path & dir.
    fs::path steamRoot = fs::path(request.dllPath).parent_path();
    fs::path cacheDir  = steamRoot / "opensteamtool" / request.channel / request.component;
    fs::path cachePath = cacheDir / (out.sha256 + ".toml");
    const std::string cachePathText = cachePath.string();

    std::error_code mkdirEc;
    fs::create_directories(cacheDir, mkdirEc);
    if (mkdirEc) {
        LOG_WARN("RemoteToml({}/{}): could not create cache dir {} ({})",
                 request.channel, request.component, cacheDir.string(), mkdirEc.message());
    }

    // 缓存优先：pattern/IPC 文件以 DLL SHA-256 命名，内容不可变，命中直接返回，避免远端超时阻塞 Hook 安装
    {
        std::error_code ec;
        if (fs::exists(cachePath, ec) && !ec) {
            const auto sz = fs::file_size(cachePath, ec);
            if (!ec && sz > 0) {
                std::ifstream ifs(cachePath, std::ios::binary);
                if (ifs) {
                    std::string buf((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
                    if (!buf.empty()) {
                        LOG_INFO("RemoteToml({}/{}): loaded from cache {}",
                                 request.channel, request.component, cachePathText);
                        out.body = std::move(buf);
                        out.ok = true;
                        out.fromCache = true;
                        return out;
                    }
                }
                LOG_WARN("RemoteToml({}/{}): cache file exists but failed to read or empty: {}",
                         request.channel, request.component, cachePathText);
            }
        }
    }

    // 3. Cache miss -> try remote (poll all mirrors in order).
    // 逐个尝试全部镜像才判定失败：CDN 同步存在延迟，首个镜像 404 时后一个镜像可能已有文件。
    // transport 失败（非 404）每个镜像多打一次：跨国链路抖一下很常见，一次重试专治抖动；
    // 404 不重试（重下也是没有）。单次重试封顶，不在启动路径上烧时间。
    const std::vector<std::string> urlTemplates = BuildUrlTemplates();
    OSTPlatform::Http::Result http;
    std::string lastUrl;
    size_t notFoundCount = 0;

    for (size_t i = 0; i < urlTemplates.size(); ++i) {
        lastUrl = ExpandTemplate(urlTemplates[i], request, out.sha256);
        LOG_INFO("RemoteToml({}/{}): downloading {}",
                 request.channel, request.component, lastUrl);

        http = OSTPlatform::Http::Execute(L"GET", lastUrl.c_str(),
                                          nullptr, 0, nullptr);

        const bool is404 = http.ok && http.status == 404;
        const bool gotBody = http.ok && http.status == 200 && !http.body.empty();
        if (!gotBody && !is404) {
            LOG_INFO("RemoteToml({}/{}): transport failed ({} ok={} HTTP={}), retrying once",
                     request.channel, request.component, lastUrl, http.ok, http.status);
            http = OSTPlatform::Http::Execute(L"GET", lastUrl.c_str(),
                                              nullptr, 0, nullptr);
        }

        if (http.ok && http.status == 200 && !http.body.empty()) break;

        const bool hasNext = (i + 1 < urlTemplates.size());
        if (http.ok && http.status == 200) {
            LOG_WARN("RemoteToml({}/{}): mirror returned empty body: {}{}",
                     request.channel, request.component, lastUrl,
                     hasNext ? ", trying next mirror" : "");
        } else if (http.ok && http.status == 404) {
            ++notFoundCount;
            LOG_WARN("RemoteToml({}/{}): mirror has no such file (HTTP 404): {}{}",
                     request.channel, request.component, lastUrl,
                     hasNext ? ", trying next mirror" : "");
        } else if (hasNext) {
            LOG_WARN("RemoteToml({}/{}): mirror failed ({} ok={} HTTP={}), falling back",
                     request.channel, request.component, lastUrl, http.ok, http.status);
        }
    }

    // 4. Remote OK → write cache, return body.
    if (http.ok && http.status == 200 && !http.body.empty()) {
        // 先落临时文件再替换，保证缓存文件不会因崩溃停留在截断状态
        fs::path tmpPath = cachePath;
        tmpPath += ".tmp";
        std::error_code writeEc;
        {
            std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
            if (ofs) {
                ofs.write(http.body.data(),
                          static_cast<std::streamsize>(http.body.size()));
                ofs.close();
            }
            if (!ofs)
                writeEc = std::make_error_code(std::errc::io_error);
        }
        if (!writeEc) {
            fs::remove(cachePath, writeEc);
            if (!writeEc)
                fs::rename(tmpPath, cachePath, writeEc);
        }
        if (writeEc) {
            LOG_WARN("RemoteToml({}/{}): could not write cache {} ({})",
                     request.channel, request.component, cachePathText, writeEc.message());
            std::error_code cleanupEc;
            fs::remove(tmpPath, cleanupEc);
        } else {
            LOG_INFO("RemoteToml({}/{}): cached to {}",
                     request.channel, request.component, cachePathText);
        }
        out.body = std::move(http.body);
        out.ok = true;
        return out;
    }

    // 5. Total failure — caller handles popup / degraded mode.
    // 区分全部 404 与网络故障：前者表示上游未发布，后者表示本机网络问题。
    // allNotFound 语义收紧：必须至少尝试过一个镜像且全 404 才算（模板配错导致
    // 零尝试不算"未发布"，算配置错，同样不弹窗）。
    out.allNotFound = !urlTemplates.empty() && notFoundCount == urlTemplates.size();
    if (out.allNotFound) {
        LOG_WARN("RemoteToml({}/{}): upstream has not published (all {} mirrors 404, last URL: {})",
                 request.channel, request.component, urlTemplates.size(),
                 lastUrl.empty() ? "<none>" : lastUrl);
    } else {
        LOG_WARN("RemoteToml({}/{}): no source available (last URL: {} HTTP {})",
                 request.channel, request.component,
                 lastUrl.empty() ? "<none>" : lastUrl, http.status);
    }
    return out;
}

bool Invalidate(const Request& request)
{
    namespace fs = std::filesystem;
    const std::string sha256 = SteamDiagnostics::Sha256Of(request.dllPath);
    if (sha256.empty()) return false;
    fs::path cachePath = fs::path(request.dllPath).parent_path() /
                         "opensteamtool" / request.channel / request.component /
                         (sha256 + ".toml");
    std::error_code ec;
    const bool removed = fs::remove(cachePath, ec);
    if (ec) {
        LOG_WARN("RemoteToml({}/{}): failed to remove stale cache {} ({})",
                 request.channel, request.component, cachePath.string(), ec.message());
        return false;
    }
    if (removed) {
        LOG_WARN("RemoteToml({}/{}): removed stale cache {}",
                 request.channel, request.component, cachePath.string());
    }
    return removed;
}

} // namespace RemoteToml
