#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

#include <hiredis/hiredis.h>
/**关于为什么要建立一个这个池子？
 * Redis用的是TCP长连接(RESP协议跑在TCP上)
 * 每次查询Redis都需要创建socket,connect(tcp连接),
 * 接收发送然后四次挥手
 * 当请求业务很频繁为何不一直保持连接不用每次都新建立销毁
 * 所以提前一次性创建好N条连接，放到池子里复用
 * 一条TCP长连接可以发送多条Redis命名，不用断开
 * 拿到 redisContext 之后：一条连接同一时间只能给一个线程用
 * -------------------------------------------------------
 * A 和 B 都拿到连接，A 修改了 reids，B 正好要获取，那岂不是冲突吗？
 * 在 Redis 服务端那边是单线程处理！不解释喵~先到先执行先得呢
 */




// =============================================================================
// 连接的所有权载体
//
// redisContext 是 hiredis 用 C 的 malloc 分配的，必须用 redisFree 释放。
// 直接塞进智能指针会走默认删除器 delete → 堆损坏，所以必须显式给删除器。
//
// 用 unique_ptr（而不是 shared_ptr）表达"连接由池子独占持有"：
//   · 所有权唯一 → 不会出现两个对象都认为自己该释放（双重释放）
//   · 借出时只给裸指针，所有权从不离开池子
// =============================================================================
struct RedisContextDeleter {
    void operator()(redisContext* context) const noexcept {
        if (context != nullptr) {
            redisFree(context);
        }
    }
};















//==============================================================================
// redisContext 是 hiredis 用 C 的 malloc 分配的，必须用 redisFree 释放。
// 显式给删除器。
using RedisContextPtr = std::unique_ptr<redisContext, RedisContextDeleter>;
// =============================================================================
// 连接守卫（RAII）—— 业务代码应该用这个，而不是裸的 GetConnection/FreeConnection
//
// 解决两个最容易出事的点：
//   1) 忘记归还 → 析构函数自动归还（含异常、提前 return 的所有路径）
//   2) 池子先死 → 守卫持有一份 shared_ptr<RedisConPool>，
//                 只要还有守卫存活，池子就不可能被析构
// (额...其实全局单例static生命周期挺长的，反正肯定比你这个守卫长...)
// 那守卫里的 pool_（shared_ptr）其实就是还东西的时候需要一个“指向池子的句柄”来调用而已...
// =============================================================================
//每借出一条连接，就创建一个守卫，让守卫"记住"这个裸指针
//守卫析构时把这同一个裸指针还给池子。
//守卫有RedisConPool的智能指针
//当所有shared_ptr都被析构后RedisConPool才能析构
//=================================================================================
//守卫能 new，但设计上只该作为「栈对象」或「类的成员」使用
// 让生命周期由作用域决定所以 "应该只作为局部变量/成员使用"。
// C++ 有个标准手法：重载 operator new 并 = delete。静止new出来！
//=================================================================================
// 其实主要是解决给出去的忘记归还的问题，因为如果忘记归还，那这个裸指针就一直被占用，
// 就不能再被其他线程借出，导致池子里的连接数一直不对，
//==============================================================================

class RedisConPool;
class RedisConnectionGuard {
public:
    explicit RedisConnectionGuard(std::shared_ptr<RedisConPool> pool);
    ~RedisConnectionGuard();

    // 守卫 = "独占使用一条连接"，拷贝会让两处同时持同一条连接 → 必须禁止
    RedisConnectionGuard(const RedisConnectionGuard&) = delete;
    RedisConnectionGuard& operator=(const RedisConnectionGuard&) = delete;
    RedisConnectionGuard(RedisConnectionGuard&& other) noexcept;//移动构造
    RedisConnectionGuard& operator=(RedisConnectionGuard&& other) noexcept;//移动赋值

    redisContext* get() const noexcept { return context_; }
    bool valid() const noexcept { return context_ != nullptr; }
    explicit operator bool() const noexcept { return context_ != nullptr; }
    
     // ---- 禁止堆分配：守卫只能作为栈对象 / 类的成员使用 ----
    // 参数必须是 std::size_t（new 表达式会传入对象大小），返回类型必须是 void*
    static void* operator new(std::size_t) = delete;      // 禁 new T
    static void* operator new[](std::size_t) = delete;    // 禁 new T[n]
    static void  operator delete(void*) = delete;         // 都new不出来了还要delete干啥都删完得了
    static void  operator delete[](void*) = delete;


private:
    void Release() noexcept;

    std::shared_ptr<RedisConPool> pool_;   // 保命：池子必然活得比守卫久
    redisContext* context_ = nullptr;      // 借到的连接（所有权仍在池子手里）
};




















// =============================================================================
// Redis 连接池（生产版）
//
//   1) 所有权唯一       池子独占连接（unique_ptr），业务只拿裸指针
//   2) 借出登记         idle_ / borrowed_ 两容器间"搬家"，池子随时知道谁在外面
//   3) 四个超时全有上限 connectTimeout / commandTimeout / borrowTimeout / shutdownTimeout
//   4) 取用前健康检查   test-on-borrow：坏连接不放给业务，丢弃并重建
//   5) 停机幂等         Close() 可重复调用；停机后借连接立刻返回 nullptr
//   6) 优雅关闭         析构等借出归还，超时则 release() 放弃（绝不 use-after-free）
//   7) 可观测           GetStats() 暴露空闲/借出/借超时/重建次数
// =============================================================================
class RedisConPool: public std::enable_shared_from_this<RedisConPool>
{
public:
	// 运行指标快照：出问题时先看这里
    struct Stats {
        size_t total = 0;              // 池子当前持有总数（idle + borrowed）
        size_t idle = 0;               // 空闲可借
        size_t borrowed = 0;           // 已借出未归还
        uint64_t borrowTotal = 0;      // 累计借出次数
        uint64_t borrowTimeout = 0;    // 累计借超时次数（持续增长 = 池子太小或有连接泄漏）
        uint64_t created = 0;          // 累计创建连接数
        uint64_t recreated = 0;        // 累计因健康检查失败而重建的次数
    };
	// 按值传参 + move 进成员：右值零拷贝、左值拷贝一次，是 C++11 的经典写法
	// 四个超时都有默认值，日常调用只写前 4 个参数
    RedisConPool(size_t poolSize,
                 std::string host,
                 int port,
                 std::string pwd,
                 std::chrono::milliseconds connectTimeout = std::chrono::milliseconds(2000),
                 std::chrono::milliseconds commandTimeout = std::chrono::milliseconds(2000),
                 std::chrono::milliseconds borrowTimeout = std::chrono::milliseconds(3000),
                 std::chrono::milliseconds shutdownTimeout = std::chrono::milliseconds(3000));

    ~RedisConPool();

	// 池子代表一份独占资源，禁止拷贝/移动（否则两个对象各自释放同一批连接）
    RedisConPool(const RedisConPool&) = delete;
    RedisConPool& operator=(const RedisConPool&) = delete;
    RedisConPool(RedisConPool&&) = delete;
    RedisConPool& operator=(RedisConPool&&) = delete;

	// ---- 低层接口：借 / 还 ----------------------------------------------
    // 借：返回裸指针（所有权仍在池子）；借不到返回 nullptr —— 【必须判空】
    // 还：必须成对调用；重复归还/归还别人的连接会被识别并忽略
    redisContext* GetConnection();
    void FreeConnection(redisContext* context) noexcept;

    // ---- 推荐接口：RAII 守卫（要求池子由 shared_ptr 管理）----------------
    RedisConnectionGuard AcquireConnectionGuard();

	 // ---- 停机 / 观测 ----------------------------------------------------
    void Close();                                        // 幂等：停机 + 唤醒所有等待者
    bool IsStopped() const noexcept { return is_stop_.load(std::memory_order_relaxed); }
    Stats GetStats() const;                              // 线程安全快照
    int get_remaining_quantity() const;                  // 兼容旧接口：当前空闲数

private:
 	// 建连 + AUTH + 设置命令超时；任一步失败返回 nullptr（内部已清理并打日志）
    RedisContextPtr CreateConnection();
    static timeval ToTimeval(std::chrono::milliseconds ms);

	// 运行计数器用原子变量：可以在不持锁的情况下更新（指标不该成为锁竞争点）
    struct Counters {
		//都是历史上发生过多少次
        std::atomic<uint64_t> borrowTotal{ 0 };// 累计借出成功次数
        std::atomic<uint64_t> borrowTimeout{ 0 };// 累计"借不到连接"次数
        std::atomic<uint64_t> created{ 0 };// 累计创建了多少条连接
        std::atomic<uint64_t> recreated{ 0 };// 累计重建了多少条（坏连接被替换）
    };

	size_t poolSize_;
	//为了让池子有自愈自我修复能力，可以重新connect所以存一下
	//host,port,password这些为成员变量
    std::string host_;
    int port_;
    std::string password_;

    std::chrono::milliseconds connectTimeout_;
    std::chrono::milliseconds commandTimeout_;
    std::chrono::milliseconds borrowTimeout_;
    std::chrono::milliseconds shutdownTimeout_;
	std::atomic<bool> is_stop_;

	/**
	* vector：必须一块连续内存，满了就要申请一块更大的连续内存，把所有元素拷贝过去，释放旧内存。
	* 所以在多线程环境下会有一些问题
	* deque 是分段连续数组基本不会遇到这个问题，所以使用queue
	*/

	std::queue<RedisContextPtr> idle_;                             // 空闲：所有权在这里
    std::unordered_map<redisContext*, RedisContextPtr> borrowed_;  // 借出：所有权仍在这里

	mutable std::mutex mutex_;
    std::condition_variable idle_cond_;    // 等"有空闲连接"
    std::condition_variable drain_cond_;   // 等"借出归零"（析构用）

    Counters counters_;


	

};