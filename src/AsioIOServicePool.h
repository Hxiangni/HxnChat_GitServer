#pragma once
//Google C++ Style Guide：**public 成员放在最前面，然后 protected，最后 private；每一块内部，函数放变量前面。**
 
// ============================================================================
// AsioIOServicePool —— asio 的 IO 服务池（io_context 线程池）
//
// 【它要解决什么问题】
//   一个 io_context 交给一个线程 run() 才高效。想让多个线程一起扛网络事件，
//   常见做法是：开 N 个 io_context，每个配一个线程去 run()，
//   来了新连接就轮流（round-robin）分给某一个 io_context。
//   这样连接被"摊"到 N 个线程上，能利用多核。
//
// 【本类的职责】
//   负责创建 N 个 io_context + N 个线程，并对外提供 GetIOService()，
//   让调用方按轮询方式拿到一个 io_context 去挂自己的 socket。
//
// 【继承关系】
//   Singleton<AsioIOServicePool> 是 CRTP 单例模板，全程序只有一份线程池。
// ============================================================================

#include <vector>
#include <thread>      // std::thread        ← 原代码缺失，需补（否则靠间接包含，很脆弱）
#include <cstddef>     // std::size_t        ← 原代码缺失，需补
#include <memory>      // std::unique_ptr    ← 原代码靠 Singleton.h 间接引入，建议显式写
#include <boost/asio.hpp>
#include "Singleton.h"

class AsioIOServicePool : public Singleton<AsioIOServicePool>
{
    // CRTP 经典写法：基类 Singleton 里的 "new T" 要调用子类的 private 构造函数，
    // 而 C++ 中基类默认无权访问派生类的私有成员，所以必须显式声明基类为友元。
    friend Singleton<AsioIOServicePool>;

public:
    // io_context 就是 asio 的"IO 服务"：负责监听事件、派发 handler、执行异步回调
    using IOService = boost::asio::io_context;

    // 【work 是什么 / 为什么必须有它】
    //   io_context::run() 的规则是：手里没有待处理的 handler 就立刻返回。
    //   线程池刚建好、还没挂任何连接时，run() 会马上返回 → 线程立即结束 → 池子白建。
    //   work（工作守卫）就是一张"占位牌"：只要还有一个 work 对象活着，
    //   run() 就认为"还有活要干"，即使当前没有 handler 也会【阻塞等待】而不是退出。
    //   要关闭时把 work 销毁/reset()，run() 才会返回，线程才能优雅结束。
    //
    // ⚠️ 原写法 boost::asio::io_context::work 在 Boost 1.91 已被移除，
    //    官方替代品就是下面这个 executor_work_guard，语义完全一致。
    using Work = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

    // 用 unique_ptr 独占式管着 work，靠 RAII 自动释放（不用手写 delete）
    using WorkPtr = std::unique_ptr<Work>;

    // 析构函数：通常在里面调用 Stop()，
    // 保证"对象销毁前所有线程都已退出"。顺序反了（线程还在跑、对象已没了）会直接崩溃。
    ~AsioIOServicePool();

    // 单例禁止拷贝/赋值。= delete 比"声明成 private 不实现"更清晰，
    // 报错信息也更友好（写 ConfigMgr 那两个 operator= 时你就体会到了）。
    AsioIOServicePool(const AsioIOServicePool&) = delete;
    AsioIOServicePool& operator=(const AsioIOServicePool&) = delete;

    // 使用 round-robin（轮询）的方式返回一个 io_context：
    // 每次调用下标 +1，到末尾绕回 0，实现"雨露均沾"。
    // 注意：_nextIOService++ 不是原子操作，多线程同时调用会有竞态；
    // 教程版一般不处理，严格实现应用 std::atomic<size_t> 或加锁。
    boost::asio::io_context& GetIOService();

    // 停止整个池子，标准三步：
    //   ① 把所有 work reset() 掉（否则 run() 永远不返回，线程 join 会卡死）
    //   ② 调用每个 io_context 的 stop()
    //   ③ join 所有线程，等它们真正结束
    void Stop();

private:
    // 构造函数放在 private：外界不能直接 new，只有"朋友" Singleton 能造，
    // 保证全程序只有一个实例。
    // size = 开几个 io_context（默认 2；作者注释里写的是可改用 CPU 核数
    //        std::thread::hardware_concurrency()）
    AsioIOServicePool(std::size_t size = 2/*std::thread::hardware_concurrency()*/);

    std::vector<IOService> _ioServices;     // N 个 io_context，各自独立跑
    std::vector<WorkPtr>   _works;          // N 个"占位牌"，防止 io_context 因没活干而空转退出
    std::vector<std::thread> _threads;      // N 个线程，每个线程跑一个 io_context.run()
    std::size_t _nextIOService=0;             // 轮询下标。⚠️ 记得在构造函数里初始化成 0
};
