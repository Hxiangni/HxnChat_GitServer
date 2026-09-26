#pragma once

// =============================================================================
// RedisConPool 单元测试（不需要任何测试框架，直接调用即可）
//
// 用法（在 main 里）：
//     const int failed = TestRedisConPool();
//     spdlog::info("TestRedisConPool: {} case(s) failed", failed);
//
// 前提：redis-server 正在运行，且 config.ini 的 [Redis] 段配置正确。
//
// 覆盖的场景：
//   T1 基本借还闭环（统计指标正确）
//   T2 借满后借超时（borrowTimeout 计数 + 不阻塞）
//   T3 归还时唤醒等待者（验证 notify_one 真的生效）
//   T4 停机后拒绝借用（Close 幂等 + 立刻返回）
//   T5 坏连接健康检查（test-on-borrow：丢弃并重建）
//   T6 参数校验 fail fast（poolSize == 0 抛异常）
//   T7 多线程并发借还（查死锁 / 计数错乱 / 双重释放）
//   T8 借出不还时析构（带超时放弃，不卡死、不崩溃）
// =============================================================================

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <hiredis/hiredis.h>
#include <spdlog/spdlog.h>

#include "ConfigMgr.h"
#include "RedisConPool.h"

inline int TestRedisConPool()
{
    using namespace std::chrono;

    int total = 0;
    int failed = 0;

    const auto check = [&total, &failed](bool ok, const std::string& name) {
        ++total;
        if (ok) {
            spdlog::info("[ TEST ] PASS  {}", name);
        }
        else {
            ++failed;
            spdlog::error("[ TEST ] FAIL  {}", name);
        }
        };

    // ---- 准备：从配置里读 redis 连接信息 ----
    auto& cfg = ConfigMgr::Inst();
    const std::string host = cfg["Redis"]["Host"].empty() ? "127.0.0.1" : cfg["Redis"]["Host"];
    const int port = cfg["Redis"]["Port"].empty() ? 6379 : std::stoi(cfg["Redis"]["Port"]);
    const std::string pwd = cfg["Redis"]["Passwd"];

    spdlog::info("[ TEST ] target redis: {}:{}", host, port);

    // 造池子的工厂：测试里用较短的超时，保证测试跑得快
    const auto makePool = [&](size_t size,
        milliseconds borrowT,
        milliseconds shutdownT) {
            return std::make_shared<RedisConPool>(
                size, host, port, pwd,
                milliseconds(1000),      // connectTimeout
                milliseconds(1000),      // commandTimeout
                borrowT,
                shutdownT);
        };

    // =========================================================================
    // T1 基本借还闭环
    // =========================================================================
    try {
        auto pool = makePool(2, milliseconds(1000), milliseconds(1000));

        auto stats0 = pool->GetStats();
        check(stats0.total == 2 && stats0.idle == 2 && stats0.borrowed == 0,
            "T1 初始状态: total=2 idle=2 borrowed=0");

        {
            auto guard = pool->AcquireConnectionGuard();
            check(static_cast<bool>(guard), "T1 AcquireConnectionGuard 拿到非空守卫");

            auto stats1 = pool->GetStats();
            check(stats1.idle == 1 && stats1.borrowed == 1,
                "T1 借出后: idle=1 borrowed=1");

            // 借到的连接真的能用
            redisReply* reply = (redisReply*)redisCommand(guard.get(), "PING");
            const bool pingOk = (reply != nullptr && reply->type == REDIS_REPLY_STATUS);
            freeReplyObject(reply);
            check(pingOk, "T1 借到的连接可以执行 PING");
        }   // 守卫析构 → 自动归还

        auto stats2 = pool->GetStats();
        check(stats2.idle == 2 && stats2.borrowed == 0,
            "T1 归还后: idle=2 borrowed=0（自动归还生效）");
        check(stats2.borrowTotal == 1, "T1 borrowTotal == 1");
        check(pool->get_remaining_quantity() == 2, "T1 get_remaining_quantity() == 2");
    }
    catch (const std::exception& e) {
        check(false, std::string("T1 抛异常: ") + e.what());
    }

    // =========================================================================
    // T2 借满后借超时
    // =========================================================================
    try {
        auto pool = makePool(2, milliseconds(600), milliseconds(1000));

        auto g1 = pool->AcquireConnectionGuard();   // 占 1
        auto g2 = pool->AcquireConnectionGuard();   // 占 2
        check(static_cast<bool>(g1) && static_cast<bool>(g2), "T2 借满 2 条连接");

        const auto start = steady_clock::now();
        auto g3 = pool->AcquireConnectionGuard();   // 应该等 borrowTimeout 后返回空
        const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();

        check(!static_cast<bool>(g3), "T2 池子借空后返回空守卫");
        check(elapsed >= 500 && elapsed < 2000,
            "T2 借超时耗时约 600ms（实测 " + std::to_string(elapsed) + "ms）");
        check(pool->GetStats().borrowTimeout == 1, "T2 borrowTimeout 计数 == 1");
    }
    catch (const std::exception& e) {
        check(false, std::string("T2 抛异常: ") + e.what());
    }

    // =========================================================================
    // T3 归还时唤醒等待者（验证 notify_one 生效）
    // =========================================================================
    try {
        auto pool = makePool(1, milliseconds(2000), milliseconds(1000));

        std::atomic<long long> waitMs{ -1 };
        std::thread waiter;

        check(static_cast<bool>(pool->GetStats().idle == 1), "T3 初始空闲 1 条");

        // 关键顺序：主线程【先】占住唯一一条，【再】启动等待线程 —— 否则谁先拿到连接是不确定的
        {
            auto g0 = pool->AcquireConnectionGuard();
            check(static_cast<bool>(g0), "T3 主线程占住唯一连接");

            // 等待线程：会阻塞在 GetConnection 的 wait 上（borrowTimeout 是 2000ms）
            waiter = std::thread([&pool, &waitMs] {
                const auto start = steady_clock::now();
                auto g = pool->AcquireConnectionGuard();
                const auto end = steady_clock::now();
                waitMs = duration_cast<milliseconds>(end - start).count();
                });

            std::this_thread::sleep_for(milliseconds(300));   // 给 waiter 时间进入等待
        }   // ★ g0 析构 → 归还 → FreeConnection 里的 notify_one 应该立刻唤醒 waiter

        waiter.join();

        check(waitMs >= 200 && waitMs < 1500,
            "T3 等待者被及时唤醒（实测 " + std::to_string(waitMs.load()) +
            "ms，若接近 2000ms 说明 notify 失效）");
    }
    catch (const std::exception& e) {
        check(false, std::string("T3 抛异常: ") + e.what());
    }

    // =========================================================================
    // T4 停机后拒绝借用（Close 幂等）
    // =========================================================================
    try {
        auto pool = makePool(2, milliseconds(1000), milliseconds(1000));

        pool->Close();
        pool->Close();                          // 幂等：再调一次不应出问题
        check(pool->IsStopped(), "T4 Close() 后 IsStopped() == true");

        const auto start = steady_clock::now();
        auto guard = pool->AcquireConnectionGuard();
        const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();

        check(!static_cast<bool>(guard), "T4 停机后借不到连接（返回空守卫）");
        check(elapsed < 300, "T4 停机后立刻返回，不阻塞（实测 " + std::to_string(elapsed) + "ms）");
    }
    catch (const std::exception& e) {
        check(false, std::string("T4 抛异常: ") + e.what());
    }

    // =========================================================================
    // T5 坏连接健康检查：借用时发现 context->err，丢弃并重建
    // =========================================================================
    try {
        auto pool = makePool(1, milliseconds(1000), milliseconds(1000));
        const uint64_t recreated0 = pool->GetStats().recreated;

        {
            auto guard = pool->AcquireConnectionGuard();
            check(static_cast<bool>(guard), "T5 借到连接");

            // 人为把连接标记为"坏了"（模拟 redis 端断开 / 网络错误）
            // 注意：这里【不 redisFree】，只改 err 字段，避免把裸指针变成悬垂指针
            guard.get()->err = REDIS_ERR;
        }   // 归还：这条"坏连接"被放回 idle_

        // 下次借用时，健康检查应该丢弃它并重建一条新的
        {
            auto guard = pool->AcquireConnectionGuard();
            check(static_cast<bool>(guard), "T5 再次借到连接（坏连接已被替换）");

            redisReply* reply = (redisReply*)redisCommand(guard.get(), "PING");
            const bool pingOk = (reply != nullptr && reply->type == REDIS_REPLY_STATUS);
            freeReplyObject(reply);
            check(pingOk, "T5 重建出来的连接可以正常 PING");
        }

        check(pool->GetStats().recreated == recreated0 + 1, "T5 recreated 计数 +1");
    }
    catch (const std::exception& e) {
        check(false, std::string("T5 抛异常: ") + e.what());
    }

    // =========================================================================
    // T6 参数校验 fail fast：poolSize == 0 必须抛异常
    // =========================================================================
    try {
        bool threw = false;
        try {
            RedisConPool bad(0, host, port, pwd);
            (void)bad;
        }
        catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "T6 poolSize == 0 抛出 std::invalid_argument（fail fast）");
    }
    catch (const std::exception& e) {
        check(false, std::string("T6 抛异常: ") + e.what());
    }

    // =========================================================================
    // T7 多线程并发借还（死锁 / 计数 / 双重释放 的照妖镜）
    // =========================================================================
    try {
        const size_t poolSize = 4;
        auto pool = makePool(poolSize, milliseconds(3000), milliseconds(2000));

        constexpr int threadCount = 8;
        constexpr int roundsPerThread = 50;

        std::atomic<int> okCount{ 0 };
        std::atomic<int> failCount{ 0 };

        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        for (int i = 0; i < threadCount; ++i) {
            // 注意：MSVC 要求把常量也显式捕获（GCC/Clang 允许省略），所以这里把
            // roundsPerThread 按值捕获进来
            threads.emplace_back([&pool, &okCount, &failCount, roundsPerThread] {
                for (int r = 0; r < roundsPerThread; ++r) {
                    auto guard = pool->AcquireConnectionGuard();
                    if (!guard) {
                        ++failCount;          // 借不到（池子够大 + 超时够长，不该发生）
                        continue;
                    }
                    redisReply* reply = (redisReply*)redisCommand(guard.get(), "PING");
                    if (reply != nullptr && reply->type == REDIS_REPLY_STATUS) {
                        ++okCount;
                    }
                    else {
                        ++failCount;
                    }
                    freeReplyObject(reply);   // 出作用域前释放 reply
                }
                });
        }
        for (auto& t : threads) {
            t.join();
        }

        const int expected = threadCount * roundsPerThread;
        auto stats = pool->GetStats();

        check(okCount.load() == expected,
            "T7 全部 " + std::to_string(expected) + " 次 PING 成功（实际 " +
            std::to_string(okCount.load()) + "）");
        check(failCount.load() == 0, "T7 没有失败/借不到的情况");
        check(stats.borrowed == 0, "T7 所有连接都已归还（borrowed == 0）");
        check(stats.idle == poolSize, "T7 空闲连接回到 poolSize（没有泄漏在多线程里）");
        check(stats.total == poolSize, "T7 总连接数没有变化（没有重复释放/丢失）");
    }
    catch (const std::exception& e) {
        check(false, std::string("T7 抛异常: ") + e.what());
    }

    // =========================================================================
    // T8 借出不还时析构：应等 shutdownTimeout 后放弃，不卡死、不崩溃
    // =========================================================================
    try {
        const auto start = steady_clock::now();
        {
            auto pool = makePool(1, milliseconds(1000), milliseconds(500));

            // 用【低层接口】借一条并且故意不还
            //（不能用守卫：守卫持有 shared_ptr，会让池子不析构）
            redisContext* leaked = pool->GetConnection();
            check(leaked != nullptr, "T8 低层接口借到连接（故意不归还）");

        }   // pool 析构：应该等 ~500ms → 打 warning → release() 放弃
        const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();

        check(elapsed >= 400 && elapsed < 3000,
            "T8 析构等待约 500ms 后放弃（实测 " + std::to_string(elapsed) + "ms），且没有崩溃");
    }
    catch (const std::exception& e) {
        check(false, std::string("T8 抛异常: ") + e.what());
    }

    // ---- 汇总 ----
    if (failed == 0) {
        spdlog::info("[ TEST ] ===== RedisConPool: {}/{} 项通过 =====", total, total);
    }
    else {
        spdlog::error("[ TEST ] ===== RedisConPool: {} 项失败 / 共 {} 项 =====", failed, total);
    }
    return failed;
}
