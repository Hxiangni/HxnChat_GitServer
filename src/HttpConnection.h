#pragma once
#include "const.h"

//cpp的http请求从tcp开始实现
class HttpConnection :public std::enable_shared_from_this<HttpConnection>
{
	friend class LogicSystem;
public:
	/*传入的实参是右值，编译器不会调用拷贝构造，而是优先匹配移动构造来创建形参。
	用 tcp::socket&& 右值引用参数时：
	  不会用移动构造生成临时 的socket，省去一次移动；
	 但是以后只能传入右值了*/
	//HttpConnection(tcp::socket socket);

	//修改为连接池
	HttpConnection(boost::asio::io_context& ioc); 

	void Start();
	tcp::socket& GetSocket();
private:
	void CheckDeadline();
	void WriteResponse();
	void HandleReq();//处理请求
	void PreParseGetParam();
	tcp::socket _socket;

	/*创建对象时直接一次性申请 8KB 连续堆内存；
	不是上限！数据超过 8192 会自动扩容（类似 vector 扩容机制）；
	初始给 8KB 是网络编程常用优化：减少频繁内存重新分配、拷贝*/
	beast::flat_buffer _buffer{ 8192 };//用来接受数据

	//Beast 内置三种标准 Body、长度未知、流式、大体积数据文件、大页面、分段返回），分段缓冲区，自动扩容
	//不是直接存放原始二进制报文，而是结构化的 C++ 对象，把 HTTP 报文解析拆成：请求头 / 响应头 + body 主体。
	http::request<http::dynamic_body> _request;//客户端发给服务器的 HTTP 请求
	http::response<http::dynamic_body> _response;//服务器回复给客户端的 HTTP 响应

	/*创建一个 asio 定时器对象
	* steady_timer系统的单调时钟 (monotonic clock)：不受系统时间修改影响。
	* 只构造定时器，什么都不会发生；**必须调用 `.async_wait(回调)`，才向 io 多路复用注册事件，才会有回调
	* 每个 asio IO 对象 (socket、acceptor、timer) 都绑定一个executor（执行器）
	* async_wait、async_read的 lambda 回调，不会直接跑；是交给 executor 去调度执行。
	  定时器产生的回调任务，交给 socket 的那一套执行器去跑
	*/
	net::steady_timer  deadline_{
		_socket.get_executor(),//让这个定时器事件，绑定到和 socket 完全相同的执行器（同一个 io_context）。
		std::chrono::seconds(60)//设置到期时长：从定时器对象构造完成的那一刻算起，往后推 60 秒到期。
	};


	std::string _get_url;//存放URL 基础地址（域名 + 资源路径）https://api.test.com/user/info
	std::unordered_map<std::string, std::string> _get_params;//存放 GET 请求的查询参数键值对
};

