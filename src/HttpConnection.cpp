#include "HttpConnection.h"
#include "LogicSystem.h"
std::string UrlDecode(const std::string& str);
//tcp::socket只有移动构造和有参构造
/*关于执行逻辑
 第 1 步：分配 HttpConnection 对象的原始内存
		  此时只是分配了空内存，_socket 还没有被构造，不是一个合法的对象。
 第 2 步：构造函数的形参 socket 初始化（第一次移动构造）
		 用传入的右值，在构造函数的栈帧上，移动构造出形参 tcp::socket socket
		 这个形参是栈上的局部变量，完全不属于 HttpConnection 对象。
 第 3 步：执行构造函数初始化列表，构造成员 _socket（第二次移动构造）
 第 4 步：执行构造函数的函数体 {}
 第 5 步：构造函数返回，形参 socket 销毁
 */
HttpConnection::HttpConnection(tcp::socket socket) :
	_socket(std::move(socket))//移动构造
{}

//发起一次异步等待客户端发来 HTTP 请求，读完完整请求后自动调用业务处理函数。
void HttpConnection::Start()
{
	auto self = shared_from_this();

	http::async_read(_socket,
		_buffer, // 存网络原始字节流
		_request, // 存解析完成后的结构化 HTTP 请求信息
		[self](beast::error_code ec, std::size_t bytes_transferred//本次异步读取一共收到了多少字节
			) {//第四个参数为回调函数
				try {
					if (ec) {
						std::cout << "http read err is " << ec.what() << std::endl;
						return;
					}

					boost::ignore_unused(bytes_transferred);//先忽略未使用的变量
					self->HandleReq();//处理本次http请求
					self->CheckDeadline();// 开启超时检测（长时间没数据自动断开连接）
				}
				//std::exception是C++ 标准库所有常规异常的基类：
				//用在try-catch中来捕获try运行失败的异常，catch 的参数变量只有触发异常时才会被赋值
				catch (std::exception& exp) {
					std::cout << "exception is " << exp.what() << std::endl;
				}
		});
}

void HttpConnection::HandleReq() {
	//设置版本
	_response.version(_request.version());

	//设置为短链接
	//长连接不关闭通道浏览器可以在同一个连接连续发送多次Http请求
	// 短链接一问一答,下次发送请求还得完整三次握手
	_response.keep_alive(false);

	//处理get请求
	if (_request.method() == http::verb::get) {
		PreParseGetParam();//解析 URL 里的路径和 GET 参数

		//根据 URL 匹配对应业务接口，第二个参数把当前连接对象传给业务层（业务处理完要发消息给客户端）
		//返回值 success：true = 接口存在；false = 没有这个接口
		bool success = LogicSystem::GetInstance()->HandleGet(_get_url, shared_from_this());


		if (!success) {
			_response.result(http::status::not_found);
			_response.set(http::field::content_type, "text/plain");
			beast::ostream(_response.body()) << "url not found\r\n";
			WriteResponse();
			return;
		}

		//接口存在也可能出错所以不能在这里定值为ok
		//_response.result(http::status::ok);失败也会返回true然后被强制修改
		_response.set(http::field::server, "GateServer");
		WriteResponse();
		return;
	}

	if (_request.method() == http::verb::post) {
		//Post分枝要不要去掉查询参数呢跟Get一样
		//PreParseGetParam();//解析 URL 里的路径和 GET 参数

		bool success = LogicSystem::GetInstance()->HandlePost(_request.target(), shared_from_this());
		if (!success) {
			_response.result(http::status::not_found);
			_response.set(http::field::content_type, "text/plain");
			beast::ostream(_response.body()) << "url not found\r\n";
			WriteResponse();
			return;
		}

		_response.result(http::status::ok);
		_response.set(http::field::server, "GateServer");
		WriteResponse();
		return;
	}


}
/*Beast 内部把_response对象序列化成 HTTP 报文字节流。
* 向操作系统提交异步写任务给内核。
* 函数立刻返回！不会阻塞等待数据发完。等待ioc
*只有 TCP 写操作完成之后，才会进入大括号里面的回调代码！
*/
void HttpConnection::WriteResponse() {
	auto self = shared_from_this();
	_response.content_length(_response.body().size());
	http::async_write(
		_socket,// 往哪个tcp socket写
		_response,// 要发送的http响应对象
		[self](beast::error_code ec, std::size_t)
		{
			//HTTP 短连接场景常用 `shutdown_send`：我们把响应全部发完之后，关闭写方向，
			// 发送 FIN 给客户端；之后还可以读完客户端剩余字节。
			self->_socket.shutdown(tcp::socket::shutdown_send, ec);
			self->deadline_.cancel();
		});
}

/*这条连接从出生起最多活 60 秒，到期还没折腾完就直接 close 兜底（比如响应写不完、客户端不收数据导致发送缓冲阻塞）。
如果在这里 expires_after(60)，等于"每次检查时把死线往后推60s"*/
void HttpConnection::CheckDeadline() {
	auto self = shared_from_this();

	deadline_.async_wait(
		[self](beast::error_code ec)
		{
			if (!ec)
			{
				// Close socket to cancel any outstanding operation.
				self->_socket.close(ec);
			}
		});
}
//解析 HTTP GET 请求的 URL，把路由路径 和 URL 查询参数 (?key=value&a=1) 拆开，
// 参数存到`_get_params`这个 map 里面。
void HttpConnection::PreParseGetParam() {
	// 提取 URI  拿到完整的路径字符串
	//target () 返回的是请求的 URI（目标资源路径），
	auto uri = _request.target();
	// 查找查询字符串的开始位置（即 '?' 的位置）
	//找到返回?的下标,没找到返回特殊常量npos
	auto query_pos = uri.find('?');
	if (query_pos == std::string::npos) {//如果返回npos说明没找到
		_get_url = uri; //整个完整的请求路径直接赋值给接口路径变量（因为没有参数，整条路径就是路由）；
		return;//不再执行后面路径解析的代码
	}

	_get_url = uri.substr(0, query_pos);//找到就获取起始到下标

	// 从?下一位开始截取，拿到全部参数字符串
	std::string query_string = uri.substr(query_pos + 1);
	std::string key;//键和值配对
	std::string value;

	size_t pos = 0;
	while ((pos = query_string.find('&')) != std::string::npos) {
		auto pair = query_string.substr(0, pos);
		size_t eq_pos = pair.find('=');
		if (eq_pos != std::string::npos) {
			//浏览器 URL 会做 URL 编码：空格变成`%20`，中文做百分号编码。`UrlDecode()`把编码后的字符串还原成原始字符串。
			key = UrlDecode(pair.substr(0, eq_pos)); // 假设有 url_decode 函数来处理URL解码  
			value = UrlDecode(pair.substr(eq_pos + 1));
			_get_params[key] = value;
		}
		query_string.erase(0, pos + 1);
	}
	// 处理最后一个参数对（如果没有 & 分隔符）  
	if (!query_string.empty()) {
		size_t eq_pos = query_string.find('=');
		if (eq_pos != std::string::npos) {
			key = UrlDecode(query_string.substr(0, eq_pos));
			value = UrlDecode(query_string.substr(eq_pos + 1));
			_get_params[key] = value;
		}
	}
}


//char 转为16进制
unsigned char ToHex(unsigned char x)
{
	return  x > 9 ? x + 55 : x + 48;
}
unsigned char FromHex(unsigned char x)
{
	unsigned char y = 0;
	if (x >= 'A' && x <= 'Z') y = x - 'A' + 10;
	else if (x >= 'a' && x <= 'z') y = x - 'a' + 10;
	else if (x >= '0' && x <= '9') y = x - '0';
	else assert(0);
	return y;
}


//URL 里中文、空格、特殊符号不能直接传输，浏览器 / 接口会把它们转成 %XX 十六进制格式，这段代码就是手动实现标准 URL 编码。
std::string UrlEncode(const std::string& str)//编码
{
	std::string strTemp = "";
	size_t length = str.length();
	//汉字是按UTF-8 字节拆分开，逐个塞进 string、逐个循环处理高低四位编码。
	for (size_t i = 0; i < length; i++)//遍历每一个字节
	{
		//判断是否仅有数字和字母构成
		if (isalnum((unsigned char)str[i]) ||
			(str[i] == '-') ||
			(str[i] == '_') ||
			(str[i] == '.') ||
			(str[i] == '~'))
			strTemp += str[i];
		else if (str[i] == ' ') //为空字符
			strTemp += "+";
		else
		{
			//其他字符需要提前加%并且高四位和低四位分别转为16进制
			strTemp += '%';
			strTemp += ToHex((unsigned char)str[i] >> 4);
			strTemp += ToHex((unsigned char)str[i] & 0x0F);
		}
	}
	return strTemp;
}

std::string UrlDecode(const std::string& str)
{
	std::string strTemp = "";
	size_t length = str.length();
	for (size_t i = 0; i < length; i++)
	{
		//还原+为空
		if (str[i] == '+') strTemp += ' ';
		//遇到%将后面的两个字符从16进制转为char再拼接
		else if (str[i] == '%')
		{
			assert(i + 2 < length);
			unsigned char high = FromHex((unsigned char)str[++i]);
			unsigned char low = FromHex((unsigned char)str[++i]);
			strTemp += high * 16 + low;
		}
		else strTemp += str[i];
	}
	return strTemp;
}