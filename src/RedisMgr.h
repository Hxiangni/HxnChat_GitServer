#pragma once

#include "Singleton.h"
#include "const.h"
#include <string>
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * redis原生的接口太难用了所以来进行一层封装
 *---------------------------------------------------
 * 具体流程
 * 1.redisConnect(host, port) 建立 TCP 通道（三次握手）
 *   通道通了，但服务器不让你执行任何命令，因为还没认证
 * 2.redisCommand(ctx, "AUTH %s", pwd)
 *   在这条通道上发一条 AUTH 命令来认证ctx这个连接是否和法
 *   合法了才能发送接下来的命令
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**
 * Redis 命令门面（Facade）
 *
 * 职责划分（每层只干一件事）：
 *   RedisConPool  只负责【池化】：借出 / 回收连接（它不认识任何命令）
 *   RedisMgr      只负责【命令】：每个方法 = 借一条连接 → 发命令 → 处理 reply → 归还
 *
 * 线程安全：每个方法独立借还连接，可被任意线程同时调用
 *          （每条连接同一时刻只被一个线程使用，由池子保证）
 *
 * 注意：这个类不再持有任何连接（_connect / _reply 已删除），
 *       建连、认证、关闭全部由 RedisConPool 负责。
 */
class RedisMgr : public Singleton<RedisMgr>
{
    friend class Singleton<RedisMgr>;   // 只有 Singleton 能 new 它（构造函数是 private）
public:
    ~RedisMgr() = default;

    // ---- String ----
    // Get：key 不存在 / redis 不可用 → 返回 false；成功 → true 且 value 带出结果
    bool Get(const std::string& key, std::string& value);
    bool Set(const std::string& key, const std::string& value);
    // 带过期时间的 SET（SET key value EX seconds）—— 验证码业务专用：N 秒后自动删除
    bool SetEx(const std::string& key, const std::string& value, int seconds);
    bool Del(const std::string& key);
    bool ExistsKey(const std::string& key);

    // ---- Hash ----
    bool HSet(const std::string& key, const std::string& hkey, const std::string& value);
    // 二进制安全版本：hvalue 的长度由调用方显式给出，内容可以包含 \0（存图片/序列化数据用）
    bool HSet(const char* key, const char* hkey, const char* hvalue, size_t hvaluelen);
    // field 不存在 / redis 不可用 → 返回 ""
    std::string HGet(const std::string& key, const std::string& hkey);

    // ---- List ----
    bool LPush(const std::string& key, const std::string& value);
    bool LPop(const std::string& key, std::string& value);
    bool RPush(const std::string& key, const std::string& value);
    bool RPop(const std::string& key, std::string& value);

private:
    RedisMgr() = default;               // 无参构造：Singleton<T> 内部用 new T 创建
};
