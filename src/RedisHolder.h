#pragma once

#include <memory>

class RedisConPool;

/**
 * Redis 连接池的【全局唯一入口】（装配点）
 *
 * 设计取向：
 *   RedisConPool 保持成一个"干净的普通类" —— 构造参数全部由外部注入，
 *   它自己不认识 ConfigMgr，也不依赖任何全局状态（可测试、可多实例）。
 *   而"读配置 + 只创建一次 + 提供访问"这些装配工作，统一放在这里。
 *
 * 用法：
 *   auto guard = RedisHolder::Get()->AcquireConnectionGuard();   // 业务代码唯一需要写的一行
 *   if (!guard) { 降级处理 }
 *   redisCommand(guard.get(), ...);
 *
 * 注意：业务代码请统一从 Get() 取池子，不要自己 new RedisConPool
 *      —— "全局唯一"靠这份约定 + 本类来保证。
 */
class RedisHolder
{
public:
    // 获取全局唯一的连接池：第一次调用时才真正创建（懒初始化，线程安全）。
    // 之后每次调用只返回已有的那份 shared_ptr 副本。
    static std::shared_ptr<RedisConPool> Get();

    // 主动停机（幂等）。建议在 main 结束前调用：
    // 把"清理"从不可控的静态析构期，提前到你自己能掌控的地方。
    // 若池子尚未创建，则什么都不做（不会顺手创建一个）。
    static void Shutdown();

private:
    // 静态实例本身（默认是空的 shared_ptr）
    // —— 与 Get() 分开，就是为了让 Shutdown() 在"池子还没创建"时不做任何事。
    static std::shared_ptr<RedisConPool>& Instance();
};
