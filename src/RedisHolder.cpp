#include "RedisHolder.h"

#include <mutex>
#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>

#include "ConfigMgr.h"
#include "RedisConPool.h"

// 静态实例：函数内静态变量（C++11 起初始化线程安全），默认是空 shared_ptr
// 函数 static 局部变量，在程序退出的时候，由运行时自动析构。
std::shared_ptr<RedisConPool>& RedisHolder::Instance()
{
    static std::shared_ptr<RedisConPool> instance;
    return instance;
}

std::shared_ptr<RedisConPool> RedisHolder::Get()
{
    // call_once 保证：无论多少个线程同时第一次调用，
    // 配置只读一次、池子只创建一次。
    // 如果创建过程抛异常，once_flag 不会置位 —— 下次调用会重新尝试（这点很好用：
    // 比如启动时 redis 没起来，后续再调用仍有机会成功）。
    static std::once_flag createFlag;
    std::call_once(createFlag, [] {
        auto& cfg = ConfigMgr::Inst();

        // 配置缺失时给合理默认值，并在日志里提示（"静默用默认值"比直接崩更难查）
        std::string host = cfg["Redis"]["Host"];
        const std::string portText = cfg["Redis"]["Port"];
        const std::string pwd = cfg["Redis"]["Passwd"];
        const std::string sizeText = cfg["Redis"]["PoolSize"];
        if (host.empty()) {
            host = "127.0.0.1";
            spdlog::warn("[RedisHolder] config [Redis]Host missing, use default {}", host);
        }

        // 把字符串转 int，顺便给出"哪个配置项写错了"的清晰错误
        const auto toInt = [](const std::string& text, int fallback, const char* name) -> int {
            if (text.empty()) {
                return fallback;
            }
            try {
                return std::stoi(text);
            }
            catch (const std::exception&) {
                throw std::invalid_argument(std::string("[RedisHolder] invalid config: ") +
                    name + "=" + text);
            }
        };

        const int port = toInt(portText, 6379, "[Redis]Port");
        const int size = toInt(sizeText, 4, "[Redis]PoolSize");
        if (port <= 0) {
            throw std::invalid_argument("[RedisHolder] invalid config: [Redis]Port must be > 0");
        }

        // 池子在这里被创建：参数由本函数注入（池子自己不需要知道配置从哪来）
        Instance() = std::make_shared<RedisConPool>(static_cast<size_t>(size), host, port, pwd);

        // 注意：日志里不要打印密码
        spdlog::info("[RedisHolder] redis pool created: {}:{}, poolSize={}",
                     host, port, size);
    });

    return Instance();
}

void RedisHolder::Shutdown()
{
    auto& instance = Instance();     // 只取引用，不创建（Instance() 从不创建池子）
    if (instance == nullptr) {
        return;                      // 从未使用过 redis：什么都不用做
    }

    instance->Close();               // 停机 + 唤醒所有等待者（幂等）
    spdlog::info("[RedisHolder] redis pool closed");
}
