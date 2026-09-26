#pragma once

#include <cassert>
#include <string>

#include <spdlog/spdlog.h>

#include "RedisMgr.h"

// RedisMgr（命令门面）的功能测试：
//   · 连接池本身的借还/超时/停机由 TestRedisConPool 覆盖
//   · 这里只验证"命令语义"是否正确
// 注意：建连/认证/关闭由 RedisConPool 负责，这里不需要 Connect/Auth/Close
inline void TestRedisMgr()
{
	auto mgr = RedisMgr::GetInstance();

	// ---- String：SET / GET ----
	assert(mgr->Set("blogwebsite", "llfc.club"));
	std::string value;
	assert(mgr->Get("blogwebsite", value));
	assert(value == "llfc.club");
	assert(mgr->Get("nonekey", value) == false);            // key 不存在 → false

	// ---- SetEx：带过期时间（验证码业务用）----
	assert(mgr->SetEx("verify:test@qq.com", "888888", 300));
	assert(mgr->ExistsKey("verify:test@qq.com"));
	assert(mgr->Del("verify:test@qq.com"));
	assert(mgr->ExistsKey("verify:test@qq.com") == false);

	// ---- Hash：HSET / HGET ----
	assert(mgr->HSet("bloginfo", "blogwebsite", "llfc.club"));
	assert(mgr->HGet("bloginfo", "blogwebsite") != "");
	assert(mgr->Del("bloginfo"));

	// ---- List：LPUSH / RPOP / LPOP ----
	// LPUSH 从左侧依次压入 1、2、3 → 列表变成 [3, 2, 1]
	assert(mgr->LPush("lpushkey1", "lpushvalue1"));
	assert(mgr->LPush("lpushkey1", "lpushvalue2"));
	assert(mgr->LPush("lpushkey1", "lpushvalue3"));
	assert(mgr->RPop("lpushkey1", value));                  // RPOP 从右侧弹出 lpushvalue1
	assert(mgr->RPop("lpushkey1", value));                  // 再弹出 lpushvalue2
	assert(mgr->LPop("lpushkey1", value));                  // 左侧弹出 lpushvalue3
	assert(mgr->LPop("lpushkey2", value) == false);         // 不存在的 key → NIL → false

	spdlog::info("[TestRedisMgr] all assertions passed");
}
