#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN //WIN32_LEAN_AND_MEAN 含义：精简 windows.h 头文件。
#include <windows.h>//windows.h 是 Windows 超级巨大的总头文件，里面会包含巨量很少用的组件（MFC、COM、网络、GDI 等）。
#endif

#include <spdlog/spdlog.h>

#include "const.h"
#include "CServer.h"
#include "ConfigMgr.h"
#include "AsioIOServicePool.h"


#include "RedisHolder.h"
//#include "RedisTest.h"       // 旧的裸 hiredis 测试，已被 TestRedisConPool 覆盖（保留作学习记录）
#include "TestRedisMgr.h"      // RedisMgr（命令门面）测试
#include "TestRedisConPool.h"  // 连接池测试


int main()
{
	// 关键：把控制台输出代码页设置为 UTF‑8
#ifdef _WIN32
	SetConsoleOutputCP(65001);
#endif
	spdlog::set_level(spdlog::level::trace);
	try
	{
		//TestRedis();   // 在 Start 服务器之前调，先确认链路通



		auto& gCfgMgr = ConfigMgr::Inst();

		std::string gate_port_str = gCfgMgr["GateServer"]["Port"];


		unsigned short gate_port = atoi(gate_port_str.c_str());
		//字面量数字 8080 强制转换成 unsigned short 无符号短整型，再赋值给 port 变量。
		//和unsigned short port=8080;有什么区别？
		unsigned short port = gate_port;


		//TestRedisMgr();   // 测试使用连接池的连接是否封装成功

		//const int failed = TestRedisConPool(); //测试连接池的功能是否正确

		//spdlog::info("TestRedisConPool: {} case(s) failed", failed);

		/*oost.Asio 的 io_context 有两种构造：
		 1.无参构造：io_context ioc;
		 2.带整数参数构造：io_context ioc(n); n告诉 io_context 最多允许 n 个线程同时调用 ioc.run() 来处理事件。*/
		 /*io_context = 事件调度器 + 任务队列 + IO 多路复用 (epoll/kqueue/iocp)，是整个 asio 异步程序的心脏。
		  1.任务队列 (task queue)：存放已经就绪、等待执行的回调任务（signal 信号回调、socket 读写回调、定时器回调）。
			队列里每一个元素，确实是一个可调用对象（有operator()），但不是你写的原始 lambda，是 asio 封装后的 task 包装对象。
		  2.IO 事件监测内核接口：Linux 用 epoll，Windows 用 IOCP，macOS 用 kqueue；用来等待操作系统事件
		  3.监视所有注册到它上面的 I/O 对象（socket、timer、signal 等），当某个对象"就绪"时，把对应的处理任务抛到队列里执行。
		  4.可以有多个ioc,每个 io_context 都是完全独立的实例：各自拥有独立任务队列、独立的 IO 多路复用*/
		
		  // 【第 1 个 ioc】独立于线程池：专供 signal_set + acceptor（接收专用）
		// 括号里的 1 是"并发提示"，意思是"我只会用 1 个线程跑它"（即 main 线程）
		net::io_context ioc{ 1 };


		/*signal_set 构造函数，传入 ioc，注册要监听的两个信号：SIGINT(Ctrl+C)、SIGTERM(kill 进程号)
		  1.asio 内部调用系统函数 sigaction()，替换进程全局的信号处理函数
		  2.asio 自己安装的这个系统信号处理函数运行在【信号中断上下文】。
		  3.signal_set 对象绑定到 ioc；signal_set 内部会准备一个管道 (pipe)。asio 经典技巧：信号处理函数里只往管道写一个字节。
		  4.io_context 的 epoll 会监听这个管道的读端
		*/
		boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);

		/*把这个异步操作注册到 signal；告诉 signal：当收到注册信号，就执行这个 lambda。
		 1.收到Ctrl+C信号后，信号处理函数会王管道写一个字节(singal_set构造的时候上面哪个代码)
		 2.然后epoll监听这个管道收到这个字节后就会把lambda封装好放到待执行的队列中等待执行
		 3.之后 ioc 事件循环从队列取出这个任务对象，执行
2.		*/
		signals.async_wait([&ioc](const boost::system::error_code& error, int signal_number) {

			if (error) {//如果 error 存在（比如信号监听被主动取消、内部 IO 出错），直接 return，不执行停机，服务继续运行。
				return;
			}
			ioc.stop();// 收到 Ctrl+C / kill 信号，停止事件循环run的停止
			});

		//堆上创建一个 CServer 对象，自动生成一个 std::shared_ptr<CServer> 智能指针管理它
		//new和make_shared都会调用构造函数
		//make_shared返回的就是 prvalue（纯右值）所以这句的分号结束后引用计数会-1
		//所以当执行到ioc.run()时（此时没有收到连接请求）只剩下lambda按值捕获的self智能指针指向CServer对象了引用计数为1
		std::make_shared<CServer>(ioc, port)->Start();

		//ioc.run() 是阻塞式事件循环，专门处理所有注册到 ioc 上的异步事件（信号、TCP 连接、HTTP 读写），只有收到停止信号才会返回。
		//当ioc.run()执行后才会处理ioc上的异步任务，所以signals.async_wait传来的lambda回调任务一定要在ioc.run之后才会被处理！！！
		ioc.run();

		// 主动停机：把"清理"从不可控的静态析构期，提前到你能掌控的位置
		RedisHolder::Shutdown();
	}
	catch (std::exception const& e)
	{
		std::cerr << "Error: " << e.what() << std::endl;
		return EXIT_FAILURE;
	}
}
/*
启动服务器，在浏览器输入`http://localhost:8000/get_test`

会看到服务器回包`receive get_test req`

如果我们输入带参数的url请求`http://localhost:8000/get_test?key1=value1&key2=value2`

会收到服务器反馈`url not found`

所以对于get请求带参数的情况我们要实现参数解析，我们可以自己实现简单的url解析函数
*/
