#pragma once
#include <string>

namespace RemoteToml {

    struct Request {
        std::string channel;    // "pattern" or "ipc"
        std::string component;  // "steamclient" or "steamui"
        std::string dllPath;
    };

    struct Result {
        bool        ok        = false;
        bool        fromCache = false;
        std::string body;
        std::string sha256;
        // 下载失败时才有意义：所有镜像都 404 = 上游真没发布（该弹窗）；
        // false = 网络故障/超时（该静默，下次再试）。成功时恒 false。
        bool        allNotFound = false;
    };

    // 优先复用以 SHA 命名的本地缓存，缺失时按序轮询全部远端镜像，全部失败才返回空
    Result Fetch(const Request& request);

    // 删掉该请求对应的缓存文件（内容串台/过期时用），返回是否删掉过文件。
    // 下一次 Fetch 会重新走远端下载。只删文件，不碰目录。
    bool Invalidate(const Request& request);

} // namespace RemoteToml
