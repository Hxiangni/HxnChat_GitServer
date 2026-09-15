#include "AsioIOServicePool.h"
#include <spdlog/spdlog.h>
AsioIOServicePool::AsioIOServicePool(std::size_t size)
	:_ioServices(size),_works(size)
	, _nextIOService(0)
{
	//    给每个 io_context 挂 work 守卫：直接用裸 new 就地构造，0 次拷贝/移动
	//    必须按下标遍历 —— 要保证 _works[i] 配的正是 _ioServices[i]
	for (std::size_t i = 0; i < _ioServices.size(); ++i) {
		_works[i].reset(new Work(_ioServices[i].get_executor()));
	}

	// 给 N 个 io_context 各配一个线程，每个线程只干一件事：跑自己的 run()
	for (std::size_t i = 0; i < _ioServices.size(); ++i) {
		//emplace_back直接在容器内存尾部里原地构造对象，不用拷贝 / 移动，
		//push_back先在外面构造一个临时对象，再把它拷贝/移动放进vector
		//`std::thread` 有一个构造函数：接收一个**可调用对象**
		//用lambda 表达式直接在 vector 中构造一个 thread
		//std::thread(可调用对象)
		_threads.emplace_back([this, i]() {
			this->_ioServices[i].run();
			});
	}
}
AsioIOServicePool::~AsioIOServicePool() {
    Stop();
    spdlog::debug("AsioIOServicePool destructed");
}

boost::asio::io_context& AsioIOServicePool::GetIOService() {
    auto& service = _ioServices[_nextIOService++];
    if (_nextIOService == _ioServices.size()) {
        _nextIOService = 0;
    }
    return service;
}

void AsioIOServicePool::Stop() {
    for (std::size_t i = 0; i < _ioServices.size(); ++i) {
        _ioServices[i].stop();    // 停服务
        _works[i].reset();        // 撤销 work 守卫
    }

    for (auto& t : _threads) {
        if (t.joinable()) {
            t.join();
        }
    }
}

// void AsioIOServicePool::Stop(){
//     //因为仅仅执行work.reset并不能让iocontext从run的状态中退出
//     //当iocontext已经绑定了读或写的监听事件后，还需要手动stop该服务。
//     for (auto& work : _works) {
//         //把服务先停止
//         work->get_executor().context().stop();
//         work.reset();
//     }

//     for (auto& t : _threads) {
//         t.join();
//     }
// }