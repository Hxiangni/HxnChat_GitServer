#include "RedisConPool.h"

#include <stdexcept>          // std::invalid_argument

#include <spdlog/spdlog.h>

 #ifdef _WIN32
 #include <winsock2.h>
 using timeval = TIMEVAL;
 #else
 #include <sys/time.h>
 #endif

// 定义成员函数必须带【类名::】，否则编译器把它当成一个返回 RedisConPool 的普通函数
// 成员初始化列表的顺序要和头文件里的【声明顺序】一致（poolSize_ → host_ → port_ → is_stop_）

RedisConPool::RedisConPool(size_t poolSize, std::string host, int port, std::string password, 
	std::chrono::milliseconds connectTimeout, std::chrono::milliseconds commandTimeout, 
	std::chrono::milliseconds borrowTimeout, std::chrono::milliseconds shutdownTimeout)
	:
	poolSize_(poolSize),host_(std::move(host)),port_(port),
	password_(std::move(password)),
	connectTimeout_(connectTimeout),
	commandTimeout_(commandTimeout),
	borrowTimeout_(borrowTimeout),
	shutdownTimeout_(shutdownTimeout),
	is_stop_(false)
{
	//传参是0直接炸给他看
	if (poolSize_ == 0) {
    	throw std::invalid_argument("[RedisConPool] poolSize must be greater than 0");
	}

	// 构造期对象尚未发布给其他线程 → 不需要加锁
	for (size_t i = 0; i < poolSize_; i++)
	{
		// 用自己的工厂：内部负责"建连 + AUTH + 设置命令超时"
		// （不能用 RedisMgr::create_auth_connect —— 那条没有超时，
		//   你的 connectTimeout_ / commandTimeout_ 就白设计了）
		//这是构造不是赋值因为context刚在这里实例化
		// T x = f(); → C++17 保证省略 → 零移动
		RedisContextPtr context = CreateConnection();
		if (context != nullptr)
			idle_.push(std::move(context));
	}
	//poolSize是你要请求的大小
	//idle_.size()是实际创建的数量
	if (idle_.size()!=poolSize)
	{
        // //创建的连接与规定的不同尝试重新创建
		// for (; idle_.size() < poolSize_;)
		// {
		// 	RedisContextPtr context = CreateConnection();
		// 	if (context != nullptr)
		// 		idle_.push(std::move(context));
		// }
		spdlog::warn("要创建的poolSize为:{}，创建的真实的poolSize_为:{}",poolSize,idle_.size());
	}
	else {
		spdlog::info("RedisConPool已创建好创建的真实的poolSize_为:{}",idle_.size());
	}
}

RedisContextPtr RedisConPool::CreateConnection(){
	   // ① 建连（带 connectTimeout_：对端不回 SYN 时，不会把当前线程挂死）
    redisContext* raw = redisConnectWithTimeout(host_.c_str(), port_, ToTimeval(connectTimeout_));

    // 分配失败：没有对象可释放，直接返回
    if (raw == nullptr) {
        spdlog::error("[RedisConPool] create failed: allocate redisContext failed");
        return nullptr;
    }

    // ② 【关键一步】立刻交给 unique_ptr 托管！
    //    从这一行起，函数里任何 return 都会自动 redisFree —— 一句手动清理都不用写
    RedisContextPtr context(raw);

    // ③ 连接失败（redisConnect 失败时返回的也是非空对象，必须释放 → 靠上面那句自动完成）
    if (context->err != 0) {
        spdlog::error("[RedisConPool] create failed: {}", context->errstr);
        return nullptr;                      // context 出作用域 → 自动 redisFree
    }

    // ④ 认证（密码为空表示服务器没开 requirepass，跳过）
    if (!password_.empty()) {
        redisReply* reply = (redisReply*)redisCommand(context.get(), "AUTH %s", password_.c_str());

        if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
            spdlog::error("[RedisConPool] auth failed: {}",
                          reply != nullptr ? reply->str : "redis network/context error");
            freeReplyObject(reply);          // freeReplyObject(nullptr) 是 no-op，安全
            return nullptr;                  // context 自动 redisFree
        }
        freeReplyObject(reply);              // 成功路径也要释放 reply！
    }

    // ⑤ 命令级超时：以后所有命令的读写最多等 commandTimeout_
	// 设置这条连接的命令超时时间
    if (redisSetTimeout(context.get(), ToTimeval(commandTimeout_)) != REDIS_OK) {
        spdlog::warn("[RedisConPool] set command timeout failed (continue anyway)");
    }

    // ⑥ 记账累计创建的连接
    counters_.created.fetch_add(1, std::memory_order_relaxed);
    return context;
}

/*=================================================================
 * 把 C++ 的std::chrono::milliseconds（毫秒时长），
 * 转换成 C 语言老结构体 timeval，timeval 里面存秒 + 微秒
 * hiredis 的一些 API 就接收 timeval 做超时时间。
 * ==================================================================
 * struct timeval {
 *   long tv_sec;   // 秒
 *   long tv_usec;  // 微秒，1秒 = 1000000微秒
 * };
 *  ==================================================================*/
timeval RedisConPool::ToTimeval(std::chrono::milliseconds ms)
{
    timeval tv{};
    tv.tv_sec = static_cast<long>(ms.count() / 1000);            // 整数秒
    tv.tv_usec = static_cast<long>((ms.count() % 1000) * 1000);  // 余下的毫秒 → 微秒
    return tv;
}

redisContext* RedisConPool::GetConnection()
{
	RedisContextPtr context;
	 // ① 借一条空闲连接：等待有上限，绝不无限阻塞业务线程
 	{
		std::unique_lock<std::mutex> lock(this->mutex_);
		const bool hasIdle = idle_cond_.wait_for(lock,borrowTimeout_, [this] {
        if (is_stop_ == true)
            return true;
        if (!idle_.empty())
            return true;
        return false;
        });

		if (is_stop_)
		{
			spdlog::debug("获取连接失败连接池已停止");
			return nullptr;
		}

		if (!hasIdle)
		{
			//借超时或者空闲队列为空
			counters_.borrowTimeout.fetch_add(1, std::memory_order_relaxed);
			spdlog::warn("[RedisConPool] borrow timeout: no idle connection in {}ms", borrowTimeout_.count());
			return nullptr;
		}
        if(!idle_.empty())
        {
            context = std::move(idle_.front());
            idle_.pop();
        }
	}

	  // ② 健康检查：坏连接不放给业务，丢弃并重建
    if (context == nullptr || context->err != 0) {
        context.reset();                                     // 丢弃（删除器自动 redisFree）
        counters_.recreated.fetch_add(1, std::memory_order_relaxed);
        context = CreateConnection();
        if (context == nullptr) {
            spdlog::error("[RedisConPool] recreate broken connection failed, pool shrank");
            return nullptr;
        }
    }

        // ③ 登记借出：所有权仍在池子，调用方只拿裸指针
    {
        std::lock_guard<std::mutex> lock(mutex_);
        //std::memory_order_relaxed 松弛内存序
        if (is_stop_.load(std::memory_order_relaxed)) {
            return nullptr;                                  // 期间被停机：放弃
        }
        redisContext* raw = context.get();
        borrowed_.emplace(raw, std::move(context));
        counters_.borrowTotal.fetch_add(1, std::memory_order_relaxed);
        return raw;                                          // ★ 成功路径必须 return raw
    }
}

void RedisConPool::FreeConnection(redisContext* context) noexcept
{
    if (context == nullptr) {
        return;                       // 容错：借的时候可能是空，还的时候静默返回
    }

    bool notifyIdle = false;          // 锁内决定、锁外执行
    //为什么在锁外执行？因为你他喵的没释放锁，就唤醒我我还是抢不到锁啊！
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = borrowed_.find(context);
        if (it == borrowed_.end()) {
            // 还两次会找不到，或者还了一个不是自己池子借出去的东西
            spdlog::error("[RedisConPool] FreeConnection: unknown or already-returned connection {}",
                          static_cast<const void*>(context));
            return;
        }

        RedisContextPtr holder = std::move(it->second);   // 先把所有权搬出来
        borrowed_.erase(it);

        if (is_stop_.load(std::memory_order_relaxed)) {
            // 停机中：不放回队列 —— holder 出作用域即 redisFree
        }
        else {
            idle_.push(std::move(holder));                // 所有权搬回空闲队列
            notifyIdle = true;
        }
    }                                 // ★ 出锁

    if (notifyIdle) {
        idle_cond_.notify_one();      // 唤醒一个正在等连接的线程（否则它要白等 3 秒超时）
    }

    //我把线程还了或者自己把他析构了，通知你看一下还有没有没还的
    drain_cond_.notify_all();         // 唤醒可能在等"借出归零"的析构流程（否则关机白等 3 秒）
}

RedisConnectionGuard RedisConPool::AcquireConnectionGuard()
{
   //把"池子自己"包装成守卫交出去
   //创建一个守卫，守卫的智能指针指向池子自己
   //调用RedisConnectionGuard的构造，获取连接
    return RedisConnectionGuard(shared_from_this());
}

RedisConnectionGuard::RedisConnectionGuard(std::shared_ptr<RedisConPool> pool)
    : pool_(std::move(pool))          // 按值传参 + move 进成员：这份引用归我了
{
    // pool_ 为空说明调用方传了空指针（比如 RedisConnectionGuard(nullptr)），
    // 此时守卫表现为"空守卫"，不会崩在下一行
    context_ = (pool_ != nullptr) ? pool_->GetConnection() : nullptr;
}

RedisConnectionGuard::~RedisConnectionGuard()
{
    Release();                        // 出作用域 → 自动归还（含异常、提前 return 的所有路径）
}

//移动构造
RedisConnectionGuard::RedisConnectionGuard(RedisConnectionGuard&& other) noexcept
    : pool_(std::move(other.pool_)), context_(other.context_)
{
    other.context_ = nullptr;         // ★ 责任已转移：被移动者必须清空，否则它析构时会【再还一次】
}
//移动赋值
RedisConnectionGuard& RedisConnectionGuard::operator=(RedisConnectionGuard&& other) noexcept
{
    if (this != &other) {
        Release();                    // 先把自己手上那条还掉（否则它就漏在 borrowed_ 里了）
        pool_ = std::move(other.pool_);
        context_ = other.context_;
        other.context_ = nullptr;     // 同上：交出去的责任不能再还
    }
    return *this;
}

void RedisConnectionGuard::Release() noexcept
{
    //指向池子的智能指针不是nullptr
    //并且有连接也不是空
    if (pool_ != nullptr && context_ != nullptr) {
        pool_->FreeConnection(context_);//归还借来的连接
    }
    context_ = nullptr;               // 置空 → 可重复调用、析构后再调也无害
    //你为啥光管context_不管shared_ptr啊！为什么不关心？
    //因为pool_ 是智能指针也是成员，由编译器在成员析构时自动完成喵
    // context_ 是裸指针，它析构时什么也不做
    // 我们置空它不是"为了清理"，而是为了"改状态"。标记"这条连接已经不归我管了"
}


// =============================================================================
// RedisConPool：停机 / 析构 / 观测
// =============================================================================

void RedisConPool::Close()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        is_stop_.store(true);         // 持锁改谓词：消除"丢失唤醒"的窗口
    }
    idle_cond_.notify_all();          // 唤醒所有等连接的线程 → 它们看到停机后返回 nullptr
    drain_cond_.notify_all();         // 唤醒可能在等"借出归零"的析构流程
}

RedisConPool::~RedisConPool()
{
    Close();                          // 幂等：先停机 + 唤醒所有等待者

    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 等所有借出的连接归还，但【有上限】：某个业务线程忘了还/死锁，也不能把进程退出拖死
        const bool drained = drain_cond_.wait_for(
            lock, shutdownTimeout_, [this] { return borrowed_.empty(); });

        if (!drained) {
            // 还有连接在外面：绝对【不能】redisFree（那是 use-after-free，别人可能还在用）
            // 主动放弃所有权 → 连接泄漏但安全，进程退出时由 OS 回收，并留下日志证据
            spdlog::warn("[RedisConPool] shutdown timeout: {} connection(s) still borrowed, "
                         "abandon them to avoid use-after-free", borrowed_.size());
            for (auto& item : borrowed_) {
                item.second.release();   // 放弃所有权：不调用删除器
            }
            borrowed_.clear();
        }
    }
    // idle_ 里的 unique_ptr 随成员析构自动 redisFree —— 不需要手写任何释放代码
}

RedisConPool::Stats RedisConPool::GetStats() const
{
    Stats stats;
    // 计数器是原子的：可以在不持锁的情况下读（指标不该成为锁竞争点）
    stats.borrowTotal = counters_.borrowTotal.load(std::memory_order_relaxed);
    stats.borrowTimeout = counters_.borrowTimeout.load(std::memory_order_relaxed);
    stats.created = counters_.created.load(std::memory_order_relaxed);
    stats.recreated = counters_.recreated.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mutex_);   // 容器大小必须在锁内读
    stats.idle = idle_.size();
    stats.borrowed = borrowed_.size();
    stats.total = stats.idle + stats.borrowed;
    return stats;
}

int RedisConPool::get_remaining_quantity() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(idle_.size());
}
