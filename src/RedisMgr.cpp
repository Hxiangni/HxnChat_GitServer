#include "RedisMgr.h"

#include <cstring>          // strcmp / strlen
#include <stdexcept>
#include <string>

#include <hiredis/hiredis.h>
#include <spdlog/spdlog.h>

#include "RedisHolder.h"    // 全局唯一的池子入口
#include "RedisConPool.h"   // RedisConnectionGuard

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

// =============================================================================
// Redis 命令门面：每个方法的套路完全一致
//   ① 借守卫（借不到 = redis 不可用 → 失败）
//   ② guard.get() 发命令 → reply（hiredis 用 malloc 分配，必须 freeReplyObject）
//   ③ 按 reply 类型处理 —— 每一个出口都要释放 reply
//
// 顺带说明：这里不再有 Connect/Auth/Close —— 建连、认证、关闭全部由
// RedisConPool 负责（构造时建连+认证，析构时统一关闭）。
// =============================================================================

// ------------------------------ String ------------------------------

bool RedisMgr::Get(const std::string& key, std::string& value)
{
	auto guard = RedisHolder::Get()->AcquireConnectionGuard();
	if (!guard) {                                       // 注意：用 !guard（类没定义 operator==）
		spdlog::error("[GET] redis unavailable, key={}", key);
		return false;
	}

	redisReply* reply = (redisReply*)redisCommand(guard.get(), "GET %s", key.c_str());
	if (reply == nullptr) {                             // 网络/连接层错误（无对象可释放）
		spdlog::error("[GET] redis network error, key={}", key);
		return false;
	}

	if (reply->type == REDIS_REPLY_NIL) {               // key 不存在：正常结果，不是错误
		freeReplyObject(reply);
		return false;
	}
	if (reply->type != REDIS_REPLY_STRING) {            // 服务器错误（如 WRONGTYPE）
		spdlog::error("[GET] failed: key={}, reason={}", key,
		              reply->str ? reply->str : "unexpected reply type");
		freeReplyObject(reply);
		return false;
	}

	value = reply->str;                                 // 先拷贝
	freeReplyObject(reply);                             // 再释放
	spdlog::debug("[GET] succeeded: key={}", key);
	return true;
}

bool RedisMgr::Set(const std::string& key, const std::string& value)
{
	auto guard = RedisHolder::Get()->AcquireConnectionGuard();
	if (!guard) {
		spdlog::error("[SET] redis unavailable, key={}", key);
		return false;
	}

	redisReply* reply = (redisReply*)redisCommand(guard.get(), "SET %s %s",
	                                              key.c_str(), value.c_str());
	if (reply == nullptr) {
		spdlog::error("[SET] redis network error, key={}", key);
		return false;
	}

	// SET 成功返回 STATUS "OK"
	const bool ok = (reply->type == REDIS_REPLY_STATUS &&
	                 reply->str != nullptr &&
	                 (strcmp(reply->str, "OK") == 0 || strcmp(reply->str, "ok") == 0));
	if (!ok) {
		spdlog::error("[SET] failed: key={}, reason={}", key,
		              reply->str ? reply->str : "unexpected reply type");
		freeReplyObject(reply);
		return false;
	}

	freeReplyObject(reply);
	spdlog::debug("[SET] succeeded: key={}", key);      // 不打 value：这里将来存验证码
	return true;
}

// 验证码业务专用：N 秒后自动删除（SET key value EX seconds）
bool RedisMgr::SetEx(const std::string& key, const std::string& value, int seconds)
{
	if (seconds <= 0) {
		spdlog::error("[SETEX] invalid seconds: {}", seconds);
		return false;
	}

	auto guard = RedisHolder::Get()->AcquireConnectionGuard();
	if (!guard) {
		spdlog::error("[SETEX] redis unavailable, key={}", key);
		return false;
	}

	redisReply* reply = (redisReply*)redisCommand(guard.get(), "SET %s %s EX %d",
	                                              key.c_str(), value.c_str(), seconds);
	if (reply == nullptr) {
		spdlog::error("[SETEX] redis network error, key={}", key);
		return false;
	}

	const bool ok = (reply->type == REDIS_REPLY_STATUS &&
	                 reply->str != nullptr &&
	                 strcmp(reply->str, "OK") == 0);
	if (!ok) {
		spdlog::error("[SETEX] failed: key={}, reason={}", key,
		              reply->str ? reply->str : "unexpected reply type");
		freeReplyObject(reply);
		return false;
	}

	freeReplyObject(reply);
	spdlog::debug("[SETEX] succeeded: key={}, ttl={}s", key, seconds);
	return true;
}

bool RedisMgr::Del(const std::string& key)
{
	auto guard = RedisHolder::Get()->AcquireConnectionGuard();
	if (!guard) {
		spdlog::error("[DEL] redis unavailable, key={}", key);
		return false;
	}

	redisReply* reply = (redisReply*)redisCommand(guard.get(), "DEL %s", key.c_str());
	if (reply == nullptr) {
		spdlog::error("[DEL] redis network error, key={}", key);
		return false;
	}

	// DEL 返回被删除的 key 数量（INTEGER）：0 = key 本来就不存在，是正常结果
	if (reply->type != REDIS_REPLY_INTEGER) {
		spdlog::error("[DEL] failed: unexpected reply, key={}", key);
		freeReplyObject(reply);
		return false;
	}

	const long long deleted = reply->integer;           // 数据要在 free 之前取出来
	freeReplyObject(reply);
	spdlog::debug("[DEL] succeeded: key={}, deleted_count={}", key, deleted);
	return true;
}

bool RedisMgr::ExistsKey(const std::string& key)
{
	auto guard = RedisHolder::Get()->AcquireConnectionGuard();
	if (!guard) {
		spdlog::error("[EXISTS] redis unavailable, key={}", key);
		return false;
	}

	redisReply* reply = (redisReply*)redisCommand(guard.get(), "EXISTS %s", key.c_str());
	if (reply == nullptr) {
		spdlog::error("[EXISTS] redis network error, key={}", key);
		return false;
	}

	if (reply->type != REDIS_REPLY_INTEGER) {
		spdlog::error("[EXISTS] failed: unexpected reply, key={}", key);
		freeReplyObject(reply);
		return false;
	}

	const bool exists = (reply->integer > 0);           // 0 = 不存在：正常结果，不是错误
	freeReplyObject(reply);

	if (exists) {
		spdlog::debug("[EXISTS] key exists: key={}", key);
	}
	else {
		spdlog::debug("[EXISTS] key not found: key={}", key);
	}
	return exists;
}

// ------------------------------ Hash ------------------------------

bool RedisMgr::HSet(const std::string& key, const std::string& hkey, const std::string& value)
{
	auto guard = RedisHolder::Get()->AcquireConnectionGuard();
	if (!guard) {
		spdlog::error("[HSET] redis unavailable, key={}", key);
		return false;
	}

	redisReply* reply = (redisReply*)redisCommand(guard.get(), "HSET %s %s %s",
	                                              key.c_str(), hkey.c_str(), value.c_str());
	if (reply == nullptr) {
		spdlog::error("[HSET] redis network error, key={}", key);
		return false;
	}

	// HSET 返回新增 field 的数量（INTEGER）
	if (reply->type != REDIS_REPLY_INTEGER) {
		spdlog::error("[HSET] failed: unexpected reply, key={}, hkey={}", key, hkey);
		freeReplyObject(reply);
		return false;
	}

	freeReplyObject(reply);
	spdlog::debug("[HSET] succeeded: key={}, hkey={}", key, hkey);
	return true;
}

// 二进制安全版本：用 redisCommandArgv，每个参数由（指针 + 长度）描述，可包含 \0
bool RedisMgr::HSet(const char* key, const char* hkey, const char* hvalue, size_t hvaluelen)
{
	auto guard = RedisHolder::Get()->AcquireConnectionGuard();
	if (!guard) {
		spdlog::error("[HSET] redis unavailable, key={}", key);
		return false;
	}

	const char* argv[4];
	size_t argvlen[4];
	argv[0] = "HSET";
	argvlen[0] = 4;                     // 第 0 个参数是命令名本身
	argv[1] = key;
	argvlen[1] = strlen(key);
	argv[2] = hkey;
	argvlen[2] = strlen(hkey);
	argv[3] = hvalue;
	argvlen[3] = hvaluelen;             // 长度由调用方给出 → 二进制安全

	redisReply* reply = (redisReply*)redisCommandArgv(guard.get(), 4, argv, argvlen);
	if (reply == nullptr) {
		spdlog::error("[HSET] redis network error, key={}", key);
		return false;
	}

	if (reply->type != REDIS_REPLY_INTEGER) {
		spdlog::error("[HSET] failed: unexpected reply, key={}, hkey={}", key, hkey);
		freeReplyObject(reply);
		return false;
	}

	freeReplyObject(reply);
	spdlog::debug("[HSET] succeeded: key={}, hkey={}, value_len={}", key, hkey, hvaluelen);
	return true;
}

std::string RedisMgr::HGet(const std::string& key, const std::string& hkey)
{
	auto guard = RedisHolder::Get()->AcquireConnectionGuard();
	if (!guard) {
		spdlog::error("[HGET] redis unavailable, key={}", key);
		return "";
	}

	redisReply* reply = (redisReply*)redisCommand(guard.get(), "HGET %s %s",
	                                              key.c_str(), hkey.c_str());
	if (reply == nullptr) {
		spdlog::error("[HGET] redis network error, key={}", key);
		return "";
	}

	if (reply->type == REDIS_REPLY_NIL) {               // field 不存在：正常结果
		freeReplyObject(reply);
		spdlog::debug("[HGET] field not exist: key={}, hkey={}", key, hkey);
		return "";
	}
	if (reply->type != REDIS_REPLY_STRING) {            // ⚠️ 不加这个判断，WRONGTYPE 的错误文本会被当成值返回
		spdlog::error("[HGET] failed: unexpected reply, key={}, hkey={}", key, hkey);
		freeReplyObject(reply);
		return "";
	}

	// 先拷贝（用 指针+长度 构造才是二进制安全的拷贝：std::string(reply->str, reply->len)）
	std::string value = reply->str;
	freeReplyObject(reply);                             // 再释放
	spdlog::debug("[HGET] succeeded: key={}, hkey={}", key, hkey);
	return value;
}

// ------------------------------ List ------------------------------

bool RedisMgr::LPush(const std::string& key, const std::string& value)
{
	auto guard = RedisHolder::Get()->AcquireConnectionGuard();
	if (!guard) {
		spdlog::error("[LPUSH] redis unavailable, key={}", key);
		return false;
	}

	redisReply* reply = (redisReply*)redisCommand(guard.get(), "LPUSH %s %s",
	                                              key.c_str(), value.c_str());
	if (reply == nullptr) {
		spdlog::error("[LPUSH] redis network error, key={}", key);
		return false;
	}

	// LPUSH 返回压入后的列表长度（INTEGER 且 >= 1）
	if (reply->type != REDIS_REPLY_INTEGER || reply->integer <= 0) {
		spdlog::error("[LPUSH] failed: unexpected reply, key={}", key);
		freeReplyObject(reply);
		return false;
	}

	const long long len = reply->integer;               // 数据要在 free 之前取出来
	freeReplyObject(reply);
	spdlog::debug("[LPUSH] succeeded: key={}, list_len={}", key, len);
	return true;
}

bool RedisMgr::LPop(const std::string& key, std::string& value)
{
	auto guard = RedisHolder::Get()->AcquireConnectionGuard();
	if (!guard) {
		spdlog::error("[LPOP] redis unavailable, key={}", key);
		return false;
	}

	redisReply* reply = (redisReply*)redisCommand(guard.get(), "LPOP %s", key.c_str());
	if (reply == nullptr) {
		spdlog::error("[LPOP] redis network error, key={}", key);
		return false;
	}

	if (reply->type == REDIS_REPLY_NIL) {               // 列表为空：正常结果
		freeReplyObject(reply);
		spdlog::debug("[LPOP] list is empty, key={}", key);
		return false;
	}
	if (reply->type != REDIS_REPLY_STRING) {            // 意外类型（如这个 key 其实是 hash）
		spdlog::error("[LPOP] failed: unexpected reply, key={}", key);
		freeReplyObject(reply);
		return false;
	}

	value = reply->str;                                 // 先拷贝
	freeReplyObject(reply);                             // 再释放
	spdlog::debug("[LPOP] succeeded: key={}, value={}", key, value);
	return true;
}

bool RedisMgr::RPush(const std::string& key, const std::string& value)
{
	auto guard = RedisHolder::Get()->AcquireConnectionGuard();
	if (!guard) {
		spdlog::error("[RPUSH] redis unavailable, key={}", key);
		return false;
	}

	redisReply* reply = (redisReply*)redisCommand(guard.get(), "RPUSH %s %s",
	                                              key.c_str(), value.c_str());
	if (reply == nullptr) {
		spdlog::error("[RPUSH] redis network error, key={}", key);
		return false;
	}

	if (reply->type != REDIS_REPLY_INTEGER || reply->integer <= 0) {
		spdlog::error("[RPUSH] failed: unexpected reply, key={}", key);
		freeReplyObject(reply);
		return false;
	}

	const long long len = reply->integer;
	freeReplyObject(reply);
	spdlog::debug("[RPUSH] succeeded: key={}, list_len={}", key, len);
	return true;
}

bool RedisMgr::RPop(const std::string& key, std::string& value)
{
	auto guard = RedisHolder::Get()->AcquireConnectionGuard();
	if (!guard) {
		spdlog::error("[RPOP] redis network error, key={}", key);
		return false;
	}

	redisReply* reply = (redisReply*)redisCommand(guard.get(), "RPOP %s", key.c_str());
	if (reply == nullptr) {
		spdlog::error("[RPOP] redis network error, key={}", key);
		return false;
	}

	if (reply->type == REDIS_REPLY_NIL) {               // 列表为空：正常结果
		freeReplyObject(reply);
		spdlog::debug("[RPOP] list is empty, key={}", key);
		return false;
	}
	if (reply->type != REDIS_REPLY_STRING) {
		spdlog::error("[RPOP] failed: unexpected reply, key={}", key);
		freeReplyObject(reply);
		return false;
	}

	value = reply->str;                                 // 先拷贝
	freeReplyObject(reply);                             // 再释放
	spdlog::debug("[RPOP] succeeded: key={}, value={}", key, value);
	return true;
}
