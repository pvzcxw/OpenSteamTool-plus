#include "Pipe/Features/DenuvoAuth/DenuvoAuth.h"

#include "Hook/Hooks_Package.h"
#include "Pipe/Features/DenuvoAuth/ProtectionScan.h"
#include "Pipe/ProcessInspector.h"
#include "OSTPlatform/include/Process.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include "Utils/Logging/Log.h"
#include "Utils/Tickets/AppTicket.h"
#include "Utils/Config/LuaConfig.h"

#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace PipeManager::DenuvoAuth {
namespace {

    // Scheme 1（默认）：启动微脉冲 300ms。盖住 Denuvo 离线 token 验证，
    // 同时在游戏引擎初始化存档目录前过期，存档进原生 userdata/<真SteamID>/。
    constexpr std::chrono::milliseconds kScheme1StartupPulseDuration{300};

    // Scheme 1（默认）：票据微脉冲 300ms。Denuvo 取票后立即做
    // memcmp(Ticket->SteamID, GetSteamID()) 交叉检查，300ms 高容错覆盖多 pipe 验证。
    constexpr std::chrono::milliseconds kScheme1TicketPulseDuration{300};

    // Scheme 1（默认）：出票投递脉冲 2500ms。票据字节交到 Denuvo 手里才开始计时，
    // 盖住 VM 加密运算（约 1.4s）加二次验证 burst（约 1.8s）。
    constexpr std::chrono::milliseconds kScheme1DeliveryPulseDuration{2500};

    // Scheme 2（dauth2 兼容）：启动宽限 2500ms，票据租约 3000ms。
    constexpr std::chrono::milliseconds kDAuth2StartupGraceDuration{2500};
    constexpr std::chrono::milliseconds kDAuth2TicketLeaseDuration{3000};

    // 命令行开关匹配（token 边界，比裸 strstr 严）：-dauth2/--dauth2/-dauth-2/-dauth_2//dauth2……
    bool HasCmdLineArg(std::string_view cmdLine, std::string_view arg) noexcept {
        if (cmdLine.empty() || arg.empty()) return false;
        const size_t argLen = arg.size();
        if (cmdLine.size() < argLen) return false;
        for (size_t i = 0; i <= cmdLine.size() - argLen; ++i) {
            if (i == 0 || std::isspace(static_cast<unsigned char>(cmdLine[i - 1])) ||
                cmdLine[i - 1] == '"' || cmdLine[i - 1] == '\'') {
                if (_strnicmp(cmdLine.data() + i, arg.data(), argLen) == 0) {
                    if (i + argLen == cmdLine.size()) return true;
                    const char next = cmdLine[i + argLen];
                    if (std::isspace(static_cast<unsigned char>(next)) ||
                        next == '"' || next == '\'' || next == '=' || next == ':') {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool HasCmdLineArg(const char* cmdLine, const char* arg) {
        if (!cmdLine || !arg) return false;
        return HasCmdLineArg(std::string_view(cmdLine), std::string_view(arg));
    }

    bool HasNoDenuvoArg(const char* cmdLine) {
        if (!cmdLine) return false;
        const std::string_view sv(cmdLine);
        return HasCmdLineArg(sv, "-nodenuvo") || HasCmdLineArg(sv, "-no-denuvo") ||
               HasCmdLineArg(sv, "-no_denuvo") || HasCmdLineArg(sv, "/nodenuvo") ||
               HasCmdLineArg(sv, "/no-denuvo") || HasCmdLineArg(sv, "/no_denuvo");
    }

    bool HasForcedDenuvoArg(const char* cmdLine) {
        if (!cmdLine) return false;
        const std::string_view sv(cmdLine);
        return HasCmdLineArg(sv, "-forcedenuvo") || HasCmdLineArg(sv, "-force-denuvo") ||
               HasCmdLineArg(sv, "-force_denuvo") || HasCmdLineArg(sv, "/forcedenuvo") ||
               HasCmdLineArg(sv, "/force-denuvo") || HasCmdLineArg(sv, "/force_denuvo");
    }

    bool HasDAuth2Arg(const char* cmdLine) {
        if (!cmdLine) return false;
        const std::string_view sv(cmdLine);
        return HasCmdLineArg(sv, "-dauth2") || HasCmdLineArg(sv, "--dauth2") ||
               HasCmdLineArg(sv, "-dauth-2") || HasCmdLineArg(sv, "-dauth_2") ||
               HasCmdLineArg(sv, "/dauth2") || HasCmdLineArg(sv, "/dauth-2") ||
               HasCmdLineArg(sv, "/dauth_2");
    }

    enum class Stage {
        None,
        Authorizing,
        EndAuthorization,
    };

    const char* ToString(Stage stage) {
        switch (stage) {
        case Stage::None:             return "None";
        case Stage::Authorizing:      return "Authorizing";
        case Stage::EndAuthorization: return "EndAuthorization";
        }
        return "?";
    }

    bool EqualsUniverseName(std::wstring_view lhs, std::wstring_view rhs) {
        if (lhs.size() != rhs.size()) return false;
        for (size_t i = 0; i < lhs.size(); ++i) {
            if (std::towlower(lhs[i]) != std::towlower(rhs[i])) return false;
        }
        return true;
    }

    EUniverse ParseUniverse(std::wstring_view universe) {
        if (EqualsUniverseName(universe, L"Public")) return k_EUniversePublic;
        if (EqualsUniverseName(universe, L"Beta")) return k_EUniverseBeta;
        if (EqualsUniverseName(universe, L"Internal")) return k_EUniverseInternal;
        if (EqualsUniverseName(universe, L"Dev")) return k_EUniverseDev;
        return k_EUniverseInvalid;
    }

    std::optional<uint64> GetCurrentSteamIdForDenuvoAuth() {
        uint32 accountId = 0;
        std::wstring universeName;
        const auto status = OSTPlatform::SteamCredentialStore::GetActiveUser(accountId, universeName);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_PIPE_WARN("DenuvoAuth: active Steam user unavailable ({})",
                           OSTPlatform::SteamCredentialStore::ToString(status));
            return std::nullopt;
        }

        const EUniverse universe = ParseUniverse(universeName);
        if (universe == k_EUniverseInvalid) {
            LOG_PIPE_WARN("DenuvoAuth: active Steam user has unsupported universe");
            return std::nullopt;
        }

        CSteamID steamId;
        steamId.Set(accountId, universe, k_EAccountTypeIndividual);
        return steamId.ConvertToUint64();
    }

    struct ProcessAuth {
        bool scanned = false;
        bool denuvo = false;
        bool isDAuth2 = false;
        bool startupArmed = false;
        bool scanning = false;
        uint32 pid = 0;

        // Scheme 1（默认）：多 pipe 感知的微脉冲状态机。租约内所有 pipe 看到同一身份，
        // 过期自动恢复真实身份，存档进原生目录。
        std::chrono::steady_clock::time_point scheme1Deadline{};

        // Scheme 2（-dauth2 / dauth2(appid)）：自适应时间窗口租约。
        std::chrono::steady_clock::time_point authDeadline{};

        AppId_t authorizedAppId = k_uAppIdInvalid;
        uint32 handshakeCount = 0;
        // 收尾持久化只做一次：窗口关闭被观测到时，把真实 SteamID 落盘给存档用。
        bool steamIdPersisted = false;

        [[nodiscard]] Stage CurrentStage(std::chrono::steady_clock::time_point now) const noexcept {
            if (!denuvo) return Stage::None;
            const auto deadline = isDAuth2 ? authDeadline : scheme1Deadline;
            if (deadline == std::chrono::steady_clock::time_point{}) return Stage::None;
            return now <= deadline ? Stage::Authorizing : Stage::EndAuthorization;
        }

        std::string DebugString() const {
            const auto now = std::chrono::steady_clock::now();
            const auto deadline = isDAuth2 ? authDeadline : scheme1Deadline;
            const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const auto stage = CurrentStage(now);
            return std::format("mode={} denuvo={} stage={} active={} remaining_ms={} handshakeCount={} auth_appid={} pid={}",
                               isDAuth2 ? "dauth2" : "default", denuvo, ToString(stage), now <= deadline,
                               remainingMs > 0 ? remainingMs : 0, handshakeCount, authorizedAppId, pid);
        }

        void OnHandshake(const PipeContext& ctx) {
            ++handshakeCount;

            if (ctx.appId != k_uAppIdInvalid) {
                authorizedAppId = ctx.appId;
            }
            pid = ctx.process.pid;

            if (!denuvo) {
                return;
            }

            // 启动脉冲只 armed 一次；deadline 只延长不缩短。
            // 握手计数不关窗口：多 pipe 下几次握手就满计数，只认时间窗口才可靠。
            if (!startupArmed) {
                startupArmed = true;
                const auto now = std::chrono::steady_clock::now();
                if (isDAuth2) {
                    const auto startupDeadline = now + kDAuth2StartupGraceDuration;
                    if (startupDeadline > authDeadline) {
                        authDeadline = startupDeadline;
                    }
                    LOG_PIPE_INFO("DenuvoAuth: [Scheme 2 - dauth2] startup grace window armed for pid={} (+{}ms) {}",
                                  pid, kDAuth2StartupGraceDuration.count(), this->DebugString());
                } else {
                    const auto startupDeadline = now + kScheme1StartupPulseDuration;
                    if (startupDeadline > scheme1Deadline) {
                        scheme1Deadline = startupDeadline;
                    }
                    LOG_PIPE_INFO("DenuvoAuth: [Scheme 1 - Default] startup micro-pulse armed for pid={} (+{}ms) {}",
                                  pid, kScheme1StartupPulseDuration.count(), this->DebugString());
                }
            } else {
                LOG_PIPE_DEBUG("DenuvoAuth: additional pipe connected handshakeCount={} {}",
                               handshakeCount, this->DebugString());
            }
        }

        void OnOwnershipTicketRequested() {
            if (!denuvo) return;
            const auto now = std::chrono::steady_clock::now();

            if (isDAuth2) {
                const auto newDeadline = now + kDAuth2TicketLeaseDuration;
                if (newDeadline > authDeadline) {
                    authDeadline = newDeadline;
                    LOG_PIPE_INFO("DenuvoAuth: [Scheme 2 - dauth2] ticket lease extended for pid={} (+{}ms) {}",
                                  pid, kDAuth2TicketLeaseDuration.count(), this->DebugString());
                }
            } else {
                const auto newDeadline = now + kScheme1TicketPulseDuration;
                if (newDeadline > scheme1Deadline) {
                    scheme1Deadline = newDeadline;
                    LOG_PIPE_INFO("DenuvoAuth: [Scheme 1 - Default] ownership micro-pulse armed for pid={} (+{}ms) {}",
                                  pid, kScheme1TicketPulseDuration.count(), this->DebugString());
                }
            }
        }

        void OnEncryptedTicketRequested() {
            if (!denuvo) return;
            LOG_PIPE_DEBUG("DenuvoAuth: encrypted ticket requested (0ms pollution) for pid={} {}",
                           pid, this->DebugString());
        }

        void OnEncryptedTicketDelivered() {
            if (!denuvo) return;
            const auto now = std::chrono::steady_clock::now();

            if (isDAuth2) {
                const auto newDeadline = now + kDAuth2TicketLeaseDuration;
                if (newDeadline > authDeadline) {
                    authDeadline = newDeadline;
                    LOG_PIPE_INFO("DenuvoAuth: [Scheme 2 - dauth2] encrypted ticket delivered lease extended for pid={} (+{}ms) {}",
                                  pid, kDAuth2TicketLeaseDuration.count(), this->DebugString());
                }
            } else {
                const auto newDeadline = now + kScheme1DeliveryPulseDuration;
                if (newDeadline > scheme1Deadline) {
                    scheme1Deadline = newDeadline;
                    LOG_PIPE_INFO("DenuvoAuth: [Scheme 1 - Default] encrypted ticket delivered -> activation delivery pulse armed for pid={} (+{}ms) {}",
                                  pid, kScheme1DeliveryPulseDuration.count(), this->DebugString());
                }
            }
        }

        [[nodiscard]] bool CanUseAuthorizedIdentity() const noexcept {
            if (!denuvo) return false;
            const auto deadline = isDAuth2 ? authDeadline : scheme1Deadline;
            if (deadline == std::chrono::steady_clock::time_point{}) return false;
            const auto now = std::chrono::steady_clock::now();
            return now <= deadline;
        }

        void WriteSteamIdOnEndAuthorization() const {
            if (authorizedAppId == k_uAppIdInvalid) {
                LOG_PIPE_WARN("DenuvoAuth: end authorization skipped SteamID persist without auth app");
                return;
            }

            const std::optional<uint64> steamId = GetCurrentSteamIdForDenuvoAuth();
            if (!steamId || *steamId == 0) {
                LOG_PIPE_WARN("DenuvoAuth: end authorization no current SteamID source auth_appid={}",
                               authorizedAppId);
                return;
            }

            if (AppTicket::WriteSteamID(authorizedAppId, *steamId)) {
                LOG_PIPE_INFO("DenuvoAuth: persisted end-authorization SteamID auth_appid={} steamid={}",
                              authorizedAppId, *steamId);
                return;
            }

            LOG_PIPE_WARN("DenuvoAuth: failed to persist end-authorization SteamID auth_appid={} steamid={}",
                          authorizedAppId, *steamId);
        }
    };

    std::mutex g_authMutex;
    std::condition_variable g_scanCv;
    std::unordered_map<ProcessKey, ProcessAuth, ProcessKeyHash> g_processAuth;
    std::unordered_map<PipeKey, ProcessKey, PipeKeyHash> g_pipeProcess;

    class ScanningGuard {
    public:
        explicit ScanningGuard(ProcessKey key) : key_(key), active_(true) {}
        ~ScanningGuard() {
            if (active_) {
                Dismiss();
            }
        }
        void Dismiss() noexcept {
            if (active_) {
                {
                    std::lock_guard lk(g_authMutex);
                    auto it = g_processAuth.find(key_);
                    if (it != g_processAuth.end()) {
                        it->second.scanning = false;
                    }
                }
                g_scanCv.notify_all();
                active_ = false;
            }
        }
        void DismissLocked(ProcessAuth& auth) noexcept {
            if (active_) {
                auth.scanning = false;
                g_scanCv.notify_all();
                active_ = false;
            }
        }
        ScanningGuard(const ScanningGuard&) = delete;
        ScanningGuard& operator=(const ScanningGuard&) = delete;
    private:
        ProcessKey key_;
        bool active_;
    };

    ProcessAuth* FindAuthForPipe(const PipeKey& pipeKey) {
        const auto pipeIt = g_pipeProcess.find(pipeKey);
        if (pipeIt != g_pipeProcess.end()) {
            const auto authIt = g_processAuth.find(pipeIt->second);
            if (authIt != g_processAuth.end()) return &authIt->second;
            g_pipeProcess.erase(pipeIt);
        }

        // 兜底：pipe 还没握手或映射被清掉时，按 pid 找同进程的授权。
        // 先内存里扫一遍，有候选才调 syscall 取创建时间，省一次系统调用。
        if (pipeKey.pid != 0) {
            bool hasPidCandidate = false;
            for (const auto& [procKey, _] : g_processAuth) {
                if (procKey.pid == pipeKey.pid) {
                    hasPidCandidate = true;
                    break;
                }
            }
            if (hasPidCandidate) {
                if (const auto currentCreation = ProcessInspector::GetProcessCreationTime(pipeKey.pid)) {
                    const ProcessKey activeKey{pipeKey.pid, *currentCreation};
                    const auto authIt = g_processAuth.find(activeKey);
                    if (authIt != g_processAuth.end()) {
                        g_pipeProcess[pipeKey] = activeKey;
                        return &authIt->second;
                    }
                }
            }
        }

        return nullptr;
    }

    // 收尾持久化只做一次：窗口关闭第一次被观测到时，把真实 SteamID 落盘给存档用。
    // 有效许可（真拥有或家庭共享）才持久化，借来的票不污染存档身份。
    void MaybePersistSteamIdOnWindowClose(ProcessAuth& auth) {
        if (auth.steamIdPersisted) return;
        if (!auth.denuvo || !auth.startupArmed) return;
        if (auth.CurrentStage(std::chrono::steady_clock::now()) != Stage::EndAuthorization) return;
        auth.steamIdPersisted = true;
        if (Hooks_Package::HasValidLicense(auth.authorizedAppId)) {
            auth.WriteSteamIdOnEndAuthorization();
        }
    }

} // namespace

void Apply(const PipeContext& ctx) {
    if (!ctx.gameProcess || !ctx.trackedApp) return;

    const PipeKey pipeKey = MakePipeKey(ctx.pipe);
    if (!pipeKey.IsValid()) return;

    bool needsScan = false;
    std::optional<ScanningGuard> scanGuard;
    {
        std::unique_lock lock(g_authMutex);
        auto it = g_processAuth.find(ctx.process);
        if (it != g_processAuth.end() && it->second.scanning) {
            g_scanCv.wait_for(lock, std::chrono::seconds(5), [&]() {
                auto sit = g_processAuth.find(ctx.process);
                return sit == g_processAuth.end() || !sit->second.scanning;
            });
            it = g_processAuth.find(ctx.process);
        }

        if (it == g_processAuth.end() || !it->second.scanned) {
            needsScan = true;
            g_processAuth[ctx.process].scanning = true;
            scanGuard.emplace(ctx.process);
        }
    }

    bool denuvo = false;
    bool isDAuth2 = LuaConfig::IsDAuth2(ctx.appId);

    if (needsScan) {
        bool cmdLineQueried = false;
        std::optional<std::string> cmdLine;
        auto GetCmdLine = [&]() -> const std::string* {
            if (!cmdLineQueried && ctx.process.pid != 0) {
                cmdLineQueried = true;
                cmdLine = OSTPlatform::Process::GetProcessCommandLine(ctx.process.pid);
            }
            return cmdLine ? &*cmdLine : nullptr;
        };

        if (!isDAuth2) {
            if (const auto* cmd = GetCmdLine()) {
                if (HasDAuth2Arg(cmd->c_str())) {
                    isDAuth2 = true;
                    LuaConfig::SetCmdLineDAuth2(ctx.appId, true);
                    LOG_PIPE_INFO("DenuvoAuth: detected -dauth2 in process command line for appid={}", ctx.appId);
                }
            }
        }

        bool isNoDenuvo = LuaConfig::IsNoDenuvo(ctx.appId);
        bool isForcedDenuvo = LuaConfig::IsForcedDenuvo(ctx.appId);

        if (const auto* cmd = GetCmdLine()) {
            if (!isNoDenuvo && HasNoDenuvoArg(cmd->c_str())) {
                isNoDenuvo = true;
                LuaConfig::SetCmdLineNoDenuvo(ctx.appId, true);
                LOG_PIPE_INFO("DenuvoAuth: detected -nodenuvo in process command line for appid={}", ctx.appId);
            }
            if (!isForcedDenuvo && HasForcedDenuvoArg(cmd->c_str())) {
                isForcedDenuvo = true;
                LuaConfig::SetCmdLineForcedDenuvo(ctx.appId, true);
                LOG_PIPE_INFO("DenuvoAuth: detected -forcedenuvo in process command line for appid={}", ctx.appId);
            }
        }

        if (isNoDenuvo) {
            denuvo = false;
            LOG_PIPE_INFO("DenuvoAuth: nodenuvo appid={} — skipping ProtectionScan and forcing non-Denuvo", ctx.appId);
        } else if (isForcedDenuvo) {
            denuvo = true;
            LOG_PIPE_INFO("DenuvoAuth: forcedenuvo appid={} — skipping ProtectionScan and forcing Denuvo", ctx.appId);
        } else {
            denuvo = ScanProtection(ctx.process.pid).denuvoDetected;
        }
    }

    std::lock_guard lock(g_authMutex);
    if (g_processAuth.size() >= 256) {
        std::erase_if(g_processAuth, [&](const auto& pair) {
            if (pair.first == ctx.process) return false;
            auto currentCreation = ProcessInspector::GetProcessCreationTime(pair.first.pid);
            return !currentCreation || *currentCreation != pair.first.creationTime;
        });
        std::erase_if(g_pipeProcess, [&](const auto& pair) {
            return !g_processAuth.contains(pair.second);
        });
    }
    if (g_pipeProcess.size() >= 512) {
        std::erase_if(g_pipeProcess, [&](const auto& pair) {
            if (pair.second == ctx.process) return false;
            return !g_processAuth.contains(pair.second);
        });
    }

    ProcessAuth& auth = g_processAuth[ctx.process];
    g_pipeProcess[pipeKey] = ctx.process;

    if (needsScan && !auth.scanned) {
        auth.scanned = true;
        auth.denuvo = denuvo;
        auth.isDAuth2 = isDAuth2;
    } else {
        if (isDAuth2) {
            auth.isDAuth2 = true;
        }
        LOG_PIPE_TRACE("DenuvoAuth: reusing cached protection result {} denuvo={} isDAuth2={}",
                       ctx.process.DebugString(), auth.denuvo, auth.isDAuth2);
    }

    if (scanGuard) {
        scanGuard->DismissLocked(auth);
    }

    auth.OnHandshake(ctx);
}

namespace {

    template <typename Fn>
    void DispatchAuthForPipe(const CPipeClient* pipe, AppId_t appId, Fn&& fn) {
        std::lock_guard lock(g_authMutex);
        if (pipe) {
            const PipeKey pipeKey = MakePipeKey(pipe);
            ProcessAuth* auth = FindAuthForPipe(pipeKey);
            if (auth) {
                if (auth->denuvo) {
                    if (auth->authorizedAppId == k_uAppIdInvalid && appId != k_uAppIdInvalid) {
                        auth->authorizedAppId = appId;
                    }
                    fn(*auth);
                }
                return;
            }
        }
        if (appId == k_uAppIdInvalid) return;

        // pipe 为空或没跟踪上时按 AppId 兜底：优先精确匹配，避免串到别的授权。
        // 防 PID 复用坑：只认创建时间对得上的活进程。
        for (auto& [procKey, auth] : g_processAuth) {
            if (auth.denuvo && auth.authorizedAppId == appId) {
                const auto creation = ProcessInspector::GetProcessCreationTime(procKey.pid);
                if (creation && *creation == procKey.creationTime) {
                    fn(auth);
                    return;
                }
            }
        }

        // 防串 app：InspectProcess 认出同一个 AppId 才绑定。
        for (auto& [procKey, auth] : g_processAuth) {
            if (auth.denuvo && auth.authorizedAppId == k_uAppIdInvalid) {
                const auto creation = ProcessInspector::GetProcessCreationTime(procKey.pid);
                if (creation && *creation == procKey.creationTime) {
                    const auto inspect = ProcessInspector::InspectProcess(procKey.pid);
                    const auto resolved = inspect.ResolveAppId();
                    if (resolved == appId) {
                        auth.authorizedAppId = appId;
                        fn(auth);
                        return;
                    }
                }
            }
        }
    }

} // namespace

void OnTicketRequested(const CPipeClient* pipe, AppId_t appId) {
    OnOwnershipTicketRequested(pipe, appId);
}

void OnOwnershipTicketRequested(const CPipeClient* pipe, AppId_t appId) {
    DispatchAuthForPipe(pipe, appId, [](ProcessAuth& auth) {
        auth.OnOwnershipTicketRequested();
    });
}

void OnEncryptedTicketRequested(const CPipeClient* pipe, AppId_t appId) {
    DispatchAuthForPipe(pipe, appId, [](ProcessAuth& auth) {
        auth.OnEncryptedTicketRequested();
    });
}

void OnEncryptedTicketDelivered(const CPipeClient* pipe, AppId_t appId) {
    DispatchAuthForPipe(pipe, appId, [](ProcessAuth& auth) {
        auth.OnEncryptedTicketDelivered();
    });
}


bool IsAuthorizedPipe(const CPipeClient* pipe) {
    if (!pipe) {
        LOG_PIPE_TRACE("DenuvoAuth: pipe not in authorization window: null pipe");
        return false;
    }

    const PipeKey pipeKey = MakePipeKey(pipe);
    std::lock_guard lock(g_authMutex);
    ProcessAuth* auth = FindAuthForPipe(pipeKey);
    if (!auth) {
        LOG_PIPE_TRACE("DenuvoAuth: pipe not tracked by DenuvoAuth {}", pipeKey.DebugString());
        return false;
    }

    if (!auth->CanUseAuthorizedIdentity()) {
        MaybePersistSteamIdOnWindowClose(*auth);
        LOG_PIPE_TRACE("DenuvoAuth: pipe not in authorization window {} {}", pipeKey.DebugString(), auth->DebugString());
        return false;
    }
    LOG_PIPE_DEBUG("DenuvoAuth: pipe in authorization window {} {}", pipeKey.DebugString(), auth->DebugString());
    return true;
}

bool IsDenuvoPipe(const CPipeClient* pipe) {
    if (!pipe) return false;
    const PipeKey pipeKey = MakePipeKey(pipe);
    std::lock_guard lock(g_authMutex);
    ProcessAuth* auth = FindAuthForPipe(pipeKey);
    return auth && auth->denuvo;
}

AppId_t GetAuthorizedAppId(const CPipeClient* pipe) {
    if (!pipe) return k_uAppIdInvalid;
    const PipeKey pipeKey = MakePipeKey(pipe);
    std::lock_guard lock(g_authMutex);
    ProcessAuth* auth = FindAuthForPipe(pipeKey);
    return auth ? auth->authorizedAppId : k_uAppIdInvalid;
}

} // namespace PipeManager::DenuvoAuth
